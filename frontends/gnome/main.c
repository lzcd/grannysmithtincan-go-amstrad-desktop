#include "cpcsession.h"
#include "cpcfujilink.h"
#include "debugger.h"
#include "runtime.h"

#include <adwaita.h>
#include <SDL.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_CATCH_UP_FRAMES = 4,
    MAX_AUDIO_QUEUE = CPC_AUDIO_HZ * sizeof(float) / 10,
    WEB_PORT = 11511,
    ROM_SIZE = 16 * 1024,
    SNAPSHOT_SIZE = 256 + 64 * 1024,
    LOG_LIMIT = 64 * 1024,
    TRAFFIC_LINES = 240
};

typedef struct {
    cpc_session *session;
    GtkWindow *window;
    GtkPicture *picture;
    GtkStack *display;
    GtkTextBuffer *traffic;
    GKeyFile *prefs;
    char *data, *rom, *settings, *dist;
    GSubprocess *runtime;
    SDL_GameController *controller;
    cpc_debugger debugger;
    GHashTable *held_keys;
    SDL_AudioDeviceID audio;
    gint64 next_frame;
    guint timer;
    guint refresh;
    unsigned boip_port, web_port;
    bool managed, offline, boot_config, gamepad, restart, closing;
    bool audio_failed;
    bool fuji_demo;
    cpc_fuji_link *fuji_link;
} desktop;

static void show_error(desktop *app, const char *message)
{
    if (!app->window) {
        g_warning("%s", message);
        return;
    }
    GtkWidget *dialog = gtk_message_dialog_new(app->window, GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                               "%s", message);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void stop_runtime(desktop *app);

static void trace_traffic(cpc_fuji_event event, uint64_t ticket, const uint8_t *bytes,
                           size_t size, void *data)
{
    desktop *app = data;
    const char *kind = event == CPC_FUJI_REQUEST ? "Request" :
                       event == CPC_FUJI_REPLY ? "Reply" : "Timeout";
    g_autoptr(GString) line = g_string_new("");
    g_string_append_printf(line, "#%" G_GUINT64_FORMAT "  %s", (guint64)ticket, kind);
    for (size_t i = 0; i < size; ++i) {
        g_string_append_printf(line, " %02X", bytes[i]);
    }
    g_string_append_c(line, '\n');
    GtkTextIter start, end;
    gtk_text_buffer_get_end_iter(app->traffic, &end);
    gtk_text_buffer_insert(app->traffic, &end, line->str, line->len);
    int excess = gtk_text_buffer_get_line_count(app->traffic) - TRAFFIC_LINES;
    if (excess > 0) {
        gtk_text_buffer_get_start_iter(app->traffic, &start);
        gtk_text_buffer_get_iter_at_line(app->traffic, &end, excess);
        gtk_text_buffer_delete(app->traffic, &start, &end);
    }
}

static bool attach_link(desktop *app, unsigned port)
{
    g_autofree char *number = g_strdup_printf("%u", port);
    app->fuji_link = cpc_fuji_link_create(number);
    if (app->fuji_link) {
        cpc_fuji_link_set_trace(app->fuji_link, trace_traffic, app);
    }
    return app->fuji_link != NULL;
}

static gpointer dispose_link_worker(gpointer data)
{
    cpc_fuji_link_destroy(data);
    return NULL;
}

static void drop_link(desktop *app)
{
    if (app->fuji_link) {
        g_thread_unref(g_thread_new("fuji-stop", dispose_link_worker, app->fuji_link));
        app->fuji_link = NULL;
    }
}

static void start_runtime(desktop *app);

static void runtime_exited(GObject *process, GAsyncResult *result, gpointer data)
{
    desktop *app = data;
    g_subprocess_wait_finish(G_SUBPROCESS(process), result, NULL);
    g_clear_object(&app->runtime);
    if (app->restart && !app->closing) {
        app->restart = false;
        start_runtime(app);
    }
}

static gboolean force_stop(gpointer process)
{
    g_subprocess_force_exit(process);
    return G_SOURCE_REMOVE;
}

static void stop_runtime(desktop *app)
{
    if (app->runtime) {
        g_subprocess_send_signal(app->runtime, SIGTERM);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, force_stop,
                           g_object_ref(app->runtime), g_object_unref);
    }
}

static void start_runtime(desktop *app)
{
    if (!app->managed || app->offline || !app->dist || app->runtime) {
        return;
    }
    g_autoptr(GError) error = NULL;
    app->runtime = cpc_runtime_start(app->dist, app->data, app->boip_port, app->web_port, &error);
    if (!app->runtime) {
        show_error(app, error->message);
        return;
    }
    g_subprocess_wait_async(app->runtime, NULL, runtime_exited, app);
    if (!app->fuji_link && !attach_link(app, app->boip_port)) {
        show_error(app, "Could not start the FujiNet link.");
        stop_runtime(app);
    }
}

static void poll_controller(desktop *app)
{
    if (!app->gamepad || !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        cpc_session_joystick(app->session, 0);
        return;
    }
    SDL_GameControllerUpdate();
    if (app->controller && !SDL_GameControllerGetAttached(app->controller)) {
        SDL_GameControllerClose(app->controller);
        app->controller = NULL;
    }
    for (int i = 0; !app->controller && i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            app->controller = SDL_GameControllerOpen(i);
        }
    }
    uint8_t mask = 0;
    if (app->controller && gtk_widget_has_focus(GTK_WIDGET(app->picture))) {
        SDL_GameController *pad = app->controller;
        Sint16 x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (y < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            mask |= CPC_JOY_UP;
        }
        if (y > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            mask |= CPC_JOY_DOWN;
        }
        if (x < -12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            mask |= CPC_JOY_LEFT;
        }
        if (x > 12000 || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            mask |= CPC_JOY_RIGHT;
        }
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) {
            mask |= CPC_JOY_FIRE;
        }
    }
    cpc_session_joystick(app->session, mask);
}

static void respond_to_fuji_demo(cpc_session *session)
{
    cpc_fuji_request request;
    if (cpc_session_fuji_take(session, &request) && request.size == 2 &&
        request.data[0] == CPC_FUJI_PING && request.data[1] == CPC_FUJI_PING_DATA) {
        static const uint8_t response[] = "PONG";
        cpc_session_fuji_reply(session, request.ticket, response, sizeof(response) - 1);
    }
}

static void queue_audio(const float *samples, size_t count, void *user_data)
{
    desktop *app = user_data;
    if (!app->audio || app->audio_failed) {
        return;
    }

    Uint32 size = (Uint32)(count * sizeof(*samples));
    if (SDL_GetQueuedAudioSize(app->audio) + size > MAX_AUDIO_QUEUE) {
        SDL_ClearQueuedAudio(app->audio);
    }
    if (SDL_QueueAudio(app->audio, samples, size) != 0) {
        g_warning("CPC audio stopped: %s", SDL_GetError());
        app->audio_failed = true;
    }
}

static void present_frame(desktop *app)
{
    cpc_frame frame = cpc_session_frame(app->session);
    GBytes *pixels = g_bytes_new(frame.rgba, frame.stride * (size_t)frame.height);
    GdkTexture *texture = gdk_memory_texture_new(frame.width, frame.height,
                                                  GDK_MEMORY_R8G8B8A8, pixels, frame.stride);
    gtk_picture_set_paintable(app->picture, GDK_PAINTABLE(texture));
    g_object_unref(texture);
    g_bytes_unref(pixels);
}

static gboolean advance(gpointer user_data)
{
    desktop *app = user_data;
    gint64 now = g_get_monotonic_time();
    if (!app->session || now < app->next_frame) {
        return G_SOURCE_CONTINUE;
    }

    if (now - app->next_frame >= MAX_CATCH_UP_FRAMES * CPC_FRAME_US) {
        app->next_frame = now;
        if (app->audio) {
            SDL_ClearQueuedAudio(app->audio);
        }
    }
    poll_controller(app);
    do {
        if (!cpc_session_run_frame(app->session) && app->audio) {
            SDL_ClearQueuedAudio(app->audio);
        }
        if (app->fuji_demo) {
            respond_to_fuji_demo(app->session);
        } else if (app->fuji_link) {
            cpc_fuji_link_service(app->fuji_link, app->session);
        }
        app->next_frame += CPC_FRAME_US;
    } while (app->next_frame <= now);
    present_frame(app);
    if (++app->refresh % 8 == 0) {
        cpc_debugger_refresh(&app->debugger);
    }
    return G_SOURCE_CONTINUE;
}

static void release_keys(GtkEventControllerFocus *controller, gpointer user_data)
{
    (void)controller;
    desktop *app = user_data;
    if (!app->session) {
        return;
    }
    GHashTableIter keys;
    gpointer key;
    g_hash_table_iter_init(&keys, app->held_keys);
    while (g_hash_table_iter_next(&keys, NULL, &key)) {
        cpc_session_key(app->session, GPOINTER_TO_INT(key), false);
    }
    g_hash_table_remove_all(app->held_keys);
}

static void reset_machine(desktop *app, bool config)
{
    if (!app->session) {
        return;
    }
    if (app->fuji_link) {
        cpc_fuji_link_reset(app->fuji_link);
    }
    g_hash_table_remove_all(app->held_keys);
    if (app->audio) {
        SDL_ClearQueuedAudio(app->audio);
    }
    if (config) {
        cpc_session_reset_to_config(app->session);
    } else {
        cpc_session_reset(app->session);
    }
    app->next_frame = g_get_monotonic_time();
    gtk_widget_grab_focus(GTK_WIDGET(app->picture));
}

static void reset_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    desktop *app = data;
    reset_machine(app, app->managed && !app->offline && app->boot_config && app->fuji_link);
}

static int cpc_keycode(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Left:      return 0x08;
    case GDK_KEY_Right:     return 0x09;
    case GDK_KEY_Down:      return 0x0a;
    case GDK_KEY_Up:        return 0x0b;
    case GDK_KEY_BackSpace: return 0x01;
    case GDK_KEY_Delete:    return 0x0c;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:  return 0x0d;
    case GDK_KEY_Escape:    return 0x03;
    default: {
        gunichar character = gdk_keyval_to_unicode(keyval);
        return character >= 0x20 && character <= 0x7e ? (int)character : 0;
    }
    }
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                            GdkModifierType modifiers, gpointer user_data)
{
    (void)controller;
    desktop *app = user_data;
    if (!app->session) {
        return GDK_EVENT_PROPAGATE;
    }
    if (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
        return GDK_EVENT_PROPAGATE;
    }

    int key = cpc_keycode(keyval);
    if (!key) {
        return GDK_EVENT_PROPAGATE;
    }
    if (!g_hash_table_contains(app->held_keys, GUINT_TO_POINTER(keycode))) {
        g_hash_table_insert(app->held_keys, GUINT_TO_POINTER(keycode), GINT_TO_POINTER(key));
        cpc_session_key(app->session, key, true);
    }
    return GDK_EVENT_STOP;
}

static void key_released(GtkEventControllerKey *controller, guint keyval, guint keycode,
                         GdkModifierType modifiers, gpointer user_data)
{
    (void)controller;
    (void)keyval;
    (void)modifiers;
    desktop *app = user_data;
    if (!app->session) {
        return;
    }
    gpointer hardware_key = GUINT_TO_POINTER(keycode);
    int key = GPOINTER_TO_INT(g_hash_table_lookup(app->held_keys, hardware_key));
    if (key) {
        cpc_session_key(app->session, key, false);
        g_hash_table_remove(app->held_keys, hardware_key);
    }
}

static bool open_session(desktop *app, const char *path, const char *basic)
{
    char error[512];
    cpc_session *session = cpc_session_create(path, basic, queue_audio, app, error, sizeof(error));
    if (!session) {
        show_error(app, error);
        return false;
    }
    if (app->session) {
        release_keys(NULL, app);
        cpc_session_destroy(app->session);
    }
    app->session = session;
    if (app->fuji_demo) {
        cpc_session_enable_fuji(session);
    } else if (app->fuji_link || (app->managed && !app->offline && app->dist)) {
        cpc_session_enable_live_fuji(session);
        if (app->managed && app->boot_config && app->fuji_link) {
            cpc_session_reset_to_config(session);
        }
    }
    if (app->fuji_link) {
        cpc_fuji_link_reset(app->fuji_link);
    }
    cpc_debugger_set_session(&app->debugger, session);
    if (app->display) {
        gtk_stack_set_visible_child_name(app->display, "machine");
        gtk_widget_grab_focus(GTK_WIDGET(app->picture));
    }
    app->next_frame = g_get_monotonic_time();
    return true;
}

typedef enum { IMPORT_OS, IMPORT_BASIC, OPEN_SNAPSHOT } file_action;

typedef struct {
    desktop *app;
    file_action action;
    char *os;
} file_selection;

static void choose_file(desktop *app, file_action action, char *os);

static void selected_file(GtkNativeDialog *dialog, int response, gpointer data)
{
    file_selection *selection = data;
    desktop *app = selection->app;
    g_autofree char *contents = NULL;
    gsize size = 0;
    g_autoptr(GError) error = NULL;
    if (response == GTK_RESPONSE_ACCEPT) {
        g_autoptr(GFile) file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        g_autoptr(GFileInputStream) stream = file ? g_file_read(file, NULL, &error) : NULL;
        gsize limit = selection->action == OPEN_SNAPSHOT ? SNAPSHOT_SIZE : 2 * ROM_SIZE;
        contents = g_malloc(limit + 1);
        if (!stream || !g_input_stream_read_all(G_INPUT_STREAM(stream), contents,
                                                 limit + 1, &size, NULL, &error)) {
            show_error(app, error ? error->message : "Could not open the selected file.");
        } else if (selection->action == OPEN_SNAPSHOT) {
            if (!app->session || !cpc_session_load_snapshot(app->session, contents, size)) {
                show_error(app, "Choose an uncompressed 64 KiB CPC 464 snapshot in SNA v1 or v2 format.");
            } else {
                g_hash_table_remove_all(app->held_keys);
                if (app->fuji_link) {
                    cpc_fuji_link_reset(app->fuji_link);
                }
                if (app->audio) {
                    SDL_ClearQueuedAudio(app->audio);
                }
                gtk_widget_grab_focus(GTK_WIDGET(app->picture));
            }
        } else if (selection->action == IMPORT_OS && size == ROM_SIZE) {
            choose_file(app, IMPORT_BASIC, g_steal_pointer(&contents));
        } else if ((selection->action == IMPORT_OS && size == 2 * ROM_SIZE) ||
                   (selection->action == IMPORT_BASIC && size == ROM_SIZE)) {
            char combined[2 * ROM_SIZE];
            if (selection->action == IMPORT_BASIC) {
                memcpy(combined, selection->os, ROM_SIZE);
                memcpy(combined + ROM_SIZE, contents, ROM_SIZE);
            } else {
                memcpy(combined, contents, sizeof(combined));
            }
            if (!g_file_set_contents_full(app->rom, combined, sizeof(combined),
                    G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
                show_error(app, error->message);
            } else {
                start_runtime(app);
                open_session(app, app->rom, NULL);
            }
        } else {
            show_error(app, "Choose a 32 KiB combined ROM or two 16 KiB files in OS, BASIC order.");
        }
    }
    g_free(selection->os);
    g_free(selection);
    g_object_unref(dialog);
}

static void choose_file(desktop *app, file_action action, char *os)
{
    if (action == OPEN_SNAPSHOT && !app->session) {
        show_error(app, "Import CPC firmware before opening a snapshot.");
        return;
    }
    const char *title = action == OPEN_SNAPSHOT ? "Open CPC Snapshot" :
                        action == IMPORT_BASIC ? "Select the BASIC ROM" : "Import CPC 464 Firmware";
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        title, app->window, GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
    file_selection *selection = g_new(file_selection, 1);
    *selection = (file_selection){ app, action, os };
    g_signal_connect(dialog, "response", G_CALLBACK(selected_file), selection);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void import_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    choose_file(data, IMPORT_OS, NULL);
}

static void open_uri(desktop *app, const char *uri)
{
    g_autoptr(GError) error = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
        show_error(app, error->message);
    }
}

static void show_text(desktop *app, const char *title, GtkTextBuffer *buffer)
{
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_transient_for(window, app->window);
    gtk_window_set_destroy_with_parent(window, TRUE);
    gtk_window_set_title(window, title);
    gtk_window_set_default_size(window, 720, 440);
    GtkWidget *view = gtk_text_view_new_with_buffer(buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(window, scroll);
    gtk_window_present(window);
}

static void show_log(desktop *app)
{
    g_autofree char *path = g_build_filename(app->data, "fujinet.log", NULL);
    FILE *file = g_fopen(path, "rb");
    char text[LOG_LIMIT];
    size_t length = 0;
    if (file) {
        if (fseek(file, 0, SEEK_END) == 0) {
            long size = ftell(file);
            if (fseek(file, size > LOG_LIMIT ? size - LOG_LIMIT : 0, SEEK_SET) == 0) {
                length = fread(text, 1, sizeof(text), file);
            }
        }
        fclose(file);
    }
    g_autofree char *valid = g_utf8_make_valid(text, length);
    g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buffer, length ? valid : "The FujiNet log is empty.", -1);
    show_text(app, "FujiNet Log", buffer);
}

static void restart_runtime(desktop *app)
{
    if (!app->managed || !app->dist || app->offline) {
        show_error(app, "Start a managed online session with the FujiNet-PC runtime installed.");
        return;
    }
    if (app->fuji_link) {
        cpc_fuji_link_reset(app->fuji_link);
    }
    if (app->runtime) {
        app->restart = true;
        stop_runtime(app);
    } else {
        start_runtime(app);
    }
}

typedef struct {
    desktop *app;
    GtkSwitch *boot_config, *gamepad, *online;
    GtkSpinButton *boip, *web;
} preferences;

static void save_preferences(GtkDialog *dialog, int response, gpointer data)
{
    preferences *fields = data;
    desktop *app = fields->app;
    if (response == GTK_RESPONSE_ACCEPT) {
        unsigned boip = gtk_spin_button_get_value_as_int(fields->boip);
        unsigned web = gtk_spin_button_get_value_as_int(fields->web);
        if (boip == web) {
            show_error(app, "The BoIP and web ports must be different.");
            return;
        }
        bool config = gtk_switch_get_active(fields->boot_config);
        bool gamepad = gtk_switch_get_active(fields->gamepad);
        bool online = gtk_switch_get_active(fields->online);
        g_key_file_set_boolean(app->prefs, "Desktop", "BootConfig", config);
        g_key_file_set_boolean(app->prefs, "Desktop", "Gamepad", gamepad);
        g_key_file_set_boolean(app->prefs, "Desktop", "Online", online);
        g_key_file_set_integer(app->prefs, "Desktop", "BoipPort", boip);
        g_key_file_set_integer(app->prefs, "Desktop", "WebPort", web);
        g_autoptr(GError) error = NULL;
        if (!g_key_file_save_to_file(app->prefs, app->settings, &error)) {
            show_error(app, error->message);
            return;
        }
        bool reconnect = boip != app->boip_port || web != app->web_port || online == app->offline;
        app->boot_config = config;
        app->gamepad = gamepad;
        if (app->managed && reconnect) {
            app->boip_port = boip;
            app->web_port = web;
            app->offline = !online;
            drop_link(app);
            if (app->offline) {
                app->restart = false;
                stop_runtime(app);
                if (app->session) {
                    cpc_session_disable_fuji(app->session);
                    g_hash_table_remove_all(app->held_keys);
                    if (app->audio) {
                        SDL_ClearQueuedAudio(app->audio);
                    }
                    app->next_frame = g_get_monotonic_time();
                }
            } else if (app->dist) {
                restart_runtime(app);
                if (app->session) {
                    g_hash_table_remove_all(app->held_keys);
                    cpc_session_enable_live_fuji(app->session);
                    reset_machine(app, config);
                }
            }
        }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void preference_row(GtkGrid *grid, int row, const char *label, GtkWidget *field)
{
    GtkWidget *caption = gtk_label_new(label);
    gtk_widget_set_halign(caption, GTK_ALIGN_START);
    gtk_widget_set_halign(field, GTK_ALIGN_END);
    gtk_grid_attach(grid, caption, 0, row, 1, 1);
    gtk_grid_attach(grid, field, 1, row, 1, 1);
}

static void show_preferences(desktop *app)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Preferences", app->window, GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    GtkGrid *grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_row_spacing(grid, 12);
    gtk_grid_set_column_spacing(grid, 32);
    gtk_widget_set_margin_top(GTK_WIDGET(grid), 16);
    gtk_widget_set_margin_bottom(GTK_WIDGET(grid), 16);
    gtk_widget_set_margin_start(GTK_WIDGET(grid), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(grid), 16);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), GTK_WIDGET(grid));
    preferences *fields = g_new0(preferences, 1);
    g_object_set_data_full(G_OBJECT(dialog), "cpc-preferences", fields, g_free);
    fields->app = app;
    fields->online = GTK_SWITCH(gtk_switch_new());
    fields->boot_config = GTK_SWITCH(gtk_switch_new());
    fields->gamepad = GTK_SWITCH(gtk_switch_new());
    fields->boip = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    fields->web = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_switch_set_active(fields->online, !app->offline);
    gtk_switch_set_active(fields->boot_config, app->boot_config);
    gtk_switch_set_active(fields->gamepad, app->gamepad);
    gtk_spin_button_set_value(fields->boip, app->boip_port);
    gtk_spin_button_set_value(fields->web, app->web_port);
    preference_row(grid, 0, "Start FujiNet automatically", GTK_WIDGET(fields->online));
    preference_row(grid, 1, "Boot to CONFIG", GTK_WIDGET(fields->boot_config));
    preference_row(grid, 2, "Enable gamepad", GTK_WIDGET(fields->gamepad));
    preference_row(grid, 3, "BoIP port", GTK_WIDGET(fields->boip));
    preference_row(grid, 4, "Web port", GTK_WIDGET(fields->web));
    if (!app->managed) {
        gtk_widget_set_sensitive(GTK_WIDGET(fields->online), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(fields->boip), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(fields->web), FALSE);
    }
    g_signal_connect(dialog, "response", G_CALLBACK(save_preferences), fields);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void desktop_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void)parameter;
    desktop *app = data;
    const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "basic") || g_str_equal(name, "config")) {
        reset_machine(app, g_str_equal(name, "config") && app->fuji_link);
    } else if (g_str_equal(name, "import")) {
        choose_file(app, IMPORT_OS, NULL);
    } else if (g_str_equal(name, "snapshot")) {
        choose_file(app, OPEN_SNAPSHOT, NULL);
    } else if (g_str_equal(name, "configuration")) {
        g_autofree char *url = g_strdup_printf("http://127.0.0.1:%u", app->web_port);
        open_uri(app, url);
    } else if (g_str_equal(name, "sd")) {
        g_autofree char *path = g_build_filename(app->data, "SD", NULL);
        if (g_mkdir_with_parents(path, 0700) != 0) {
            show_error(app, g_strerror(errno));
        } else {
            g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
            if (uri) {
                open_uri(app, uri);
            }
        }
    } else if (g_str_equal(name, "log")) {
        show_log(app);
    } else if (g_str_equal(name, "traffic")) {
        show_text(app, "FujiNet Traffic", app->traffic);
    } else if (g_str_equal(name, "restart")) {
        restart_runtime(app);
    } else if (g_str_equal(name, "debugger")) {
        cpc_debugger_show(&app->debugger, app->window);
    } else if (g_str_equal(name, "quit")) {
        gtk_window_close(app->window);
    } else {
        show_preferences(app);
    }
}

static gboolean close_window(GtkWindow *window, gpointer user_data)
{
    (void)window;
    desktop *app = user_data;
    app->closing = true;
    g_clear_handle_id(&app->timer, g_source_remove);
    cpc_debugger_clear(&app->debugger);
    app->window = NULL;
    return GDK_EVENT_PROPAGATE;
}

static void activate(GtkApplication *application, gpointer user_data)
{
    desktop *app = user_data;
    if (app->window) {
        gtk_window_present(app->window);
        return;
    }

    app->window = GTK_WINDOW(adw_application_window_new(application));
    gtk_window_set_title(app->window, "FujiNet Go — Amstrad CPC 464");
    gtk_window_set_default_size(app->window, 768, 590);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_window), app);

    GtkWidget *header = adw_header_bar_new();
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), app);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), reset);
    static const GActionEntry actions[] = {
        { .name = "basic", .activate = desktop_action },
        { .name = "config", .activate = desktop_action },
        { .name = "configuration", .activate = desktop_action },
        { .name = "debugger", .activate = desktop_action },
        { .name = "import", .activate = desktop_action },
        { .name = "log", .activate = desktop_action },
        { .name = "preferences", .activate = desktop_action },
        { .name = "quit", .activate = desktop_action },
        { .name = "restart", .activate = desktop_action },
        { .name = "sd", .activate = desktop_action },
        { .name = "snapshot", .activate = desktop_action },
        { .name = "traffic", .activate = desktop_action }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app->window), actions, G_N_ELEMENTS(actions), app);
    gtk_application_set_accels_for_action(application, "win.basic", (const char *[]){ "<Ctrl>b", NULL });
    gtk_application_set_accels_for_action(application, "win.config", (const char *[]){ "<Ctrl>r", NULL });
    gtk_application_set_accels_for_action(application, "win.restart", (const char *[]){ "<Ctrl><Shift>r", NULL });
    gtk_application_set_accels_for_action(application, "win.debugger", (const char *[]){ "F12", NULL });
    gtk_application_set_accels_for_action(application, "win.import", (const char *[]){ "<Ctrl>i", NULL });
    gtk_application_set_accels_for_action(application, "win.preferences", (const char *[]){ "<Ctrl>comma", NULL });
    gtk_application_set_accels_for_action(application, "win.quit", (const char *[]){ "<Ctrl>q", NULL });
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) machine = g_menu_new();
    g_autoptr(GMenu) fujinet = g_menu_new();
    g_autoptr(GMenu) tools = g_menu_new();
    g_menu_append(machine, "Reset to BASIC", "win.basic");
    g_menu_append(machine, "Reset to CONFIG", "win.config");
    g_menu_append(machine, "Import CPC Firmware…", "win.import");
    g_menu_append(machine, "Open Snapshot…", "win.snapshot");
    g_menu_append(fujinet, "Configuration", "win.configuration");
    g_menu_append(fujinet, "Open SD Folder", "win.sd");
    g_menu_append(fujinet, "FujiNet Log", "win.log");
    g_menu_append(fujinet, "FujiNet Traffic", "win.traffic");
    g_menu_append(fujinet, "Restart FujiNet", "win.restart");
    g_menu_append(tools, "Z80 Debugger", "win.debugger");
    g_menu_append(tools, "Preferences", "win.preferences");
    g_menu_append(tools, "Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(machine));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fujinet));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tools));
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    app->picture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(app->picture, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_focusable(GTK_WIDGET(app->picture), true);
    gtk_widget_set_vexpand(GTK_WIDGET(app->picture), true);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), app);
    g_signal_connect(keys, "key-released", G_CALLBACK(key_released), app);
    gtk_widget_add_controller(GTK_WIDGET(app->picture), keys);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(release_keys), app);
    gtk_widget_add_controller(GTK_WIDGET(app->picture), focus);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content), header);
    app->display = GTK_STACK(gtk_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(app->display), TRUE);
    gtk_stack_add_named(app->display, GTK_WIDGET(app->picture), "machine");
    GtkWidget *welcome = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(welcome), "computer-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(welcome), "CPC 464 Firmware");
    adw_status_page_set_description(ADW_STATUS_PAGE(welcome),
        "Choose a combined 32 KiB ROM or separate 16 KiB OS and BASIC ROMs.");
    GtkWidget *import = gtk_button_new_with_label("Import Firmware…");
    gtk_widget_add_css_class(import, "suggested-action");
    gtk_widget_set_halign(import, GTK_ALIGN_CENTER);
    g_signal_connect(import, "clicked", G_CALLBACK(import_clicked), app);
    adw_status_page_set_child(ADW_STATUS_PAGE(welcome), import);
    gtk_stack_add_named(app->display, welcome, "welcome");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(app->display));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(app->window), content);

    if (!app->session) {
        gtk_stack_set_visible_child_name(app->display, "welcome");
        const char *rom = g_file_test(app->rom, G_FILE_TEST_IS_REGULAR) ? app->rom : CPC_INSTALLED_ROM;
        if (g_file_test(rom, G_FILE_TEST_IS_REGULAR)) {
            start_runtime(app);
            open_session(app, rom, NULL);
        }
    }
    app->next_frame = g_get_monotonic_time();
    advance(app);
    app->timer = g_timeout_add(4, advance, app);
    gtk_window_present(app->window);
    if (app->session) {
        gtk_widget_grab_focus(GTK_WIDGET(app->picture));
    }
}

static bool preference_bool(GKeyFile *prefs, const char *key, bool fallback)
{
    g_autoptr(GError) error = NULL;
    bool value = g_key_file_get_boolean(prefs, "Desktop", key, &error);
    return error ? fallback : value;
}

static unsigned preference_port(GKeyFile *prefs, const char *key, unsigned fallback)
{
    int value = g_key_file_get_integer(prefs, "Desktop", key, NULL);
    return value >= 1 && value <= 65535 ? (unsigned)value : fallback;
}

static const char *default_runtime(void)
{
    const char *env = g_getenv("FUJINET_RUNTIME_DIST");
    if (env && *env) {
        return env;
    }
    if (g_file_test(CPC_INSTALLED_RUNTIME "/fujinet", G_FILE_TEST_IS_EXECUTABLE)) {
        return CPC_INSTALLED_RUNTIME;
    }
    if (g_file_test("build/fujinet-firmware/build/dist/fujinet", G_FILE_TEST_IS_EXECUTABLE)) {
        return "build/fujinet-firmware/build/dist";
    }
    return NULL;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--offline] [--runtime-dir DIR]\n"
                    "       %s [--fuji-demo] CPC464_ROM [BASIC_ROM]\n"
                    "       %s --fuji-live BOIP_PORT CPC464_ROM [BASIC_ROM]\n",
            program, program, program);
}

int main(int argc, char **argv)
{
    bool demo = argc > 1 && g_str_equal(argv[1], "--fuji-demo");
    bool live = argc > 1 && g_str_equal(argv[1], "--fuji-live");
    int rom_arg = demo ? 2 : live ? 3 : 1;
    bool offline = false;
    const char *runtime = NULL;
    while (rom_arg < argc && argv[rom_arg][0] == '-' && !demo && !live) {
        if (g_str_equal(argv[rom_arg], "--offline")) {
            offline = true;
        } else if (g_str_equal(argv[rom_arg], "--runtime-dir") && rom_arg + 1 < argc) {
            runtime = argv[++rom_arg];
        } else if (g_str_equal(argv[rom_arg], "--help")) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        ++rom_arg;
    }
    int rom_count = argc - rom_arg;
    if (rom_count > 2 || rom_count < 0 || (!rom_count && (demo || live)) ||
        (rom_count && (offline || runtime))) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    g_autofree char *data = g_build_filename(g_get_user_data_dir(), "fujinet-go-amstrad", NULL);
    if (g_mkdir_with_parents(data, 0700) != 0) {
        fprintf(stderr, "Could not create the desktop data directory: %s\n", g_strerror(errno));
        return EXIT_FAILURE;
    }
    g_autofree char *rom = g_build_filename(data, "cpc464.rom", NULL);
    g_autofree char *settings = g_build_filename(data, "desktop.ini", NULL);
    g_autoptr(GKeyFile) prefs = g_key_file_new();
    g_key_file_load_from_file(prefs, settings, G_KEY_FILE_KEEP_COMMENTS, NULL);
    const char *dist_path = runtime ? runtime : default_runtime();
    g_autofree char *dist = dist_path ? g_canonicalize_filename(dist_path, NULL) : NULL;
    g_autoptr(GtkTextBuffer) traffic = gtk_text_buffer_new(NULL);
    g_autoptr(GHashTable) held_keys = g_hash_table_new(g_direct_hash, g_direct_equal);
    desktop app = {
        .managed = rom_count == 0,
        .offline = offline || !preference_bool(prefs, "Online", true),
        .boot_config = preference_bool(prefs, "BootConfig", true),
        .gamepad = preference_bool(prefs, "Gamepad", true),
        .boip_port = preference_port(prefs, "BoipPort", CPC_FUJI_BOIP_PORT),
        .web_port = preference_port(prefs, "WebPort", WEB_PORT),
        .data = data, .rom = rom, .settings = settings, .dist = dist,
        .prefs = prefs, .traffic = traffic, .held_keys = held_keys, .fuji_demo = demo
    };
    if (live) {
        guint64 port;
        if (!g_ascii_string_to_unsigned(argv[2], 10, 1, 65535, &port, NULL) ||
            !attach_link(&app, (unsigned)port)) {
            fputs("Could not start the FujiNet link. Check the BoIP port.\n", stderr);
            return EXIT_FAILURE;
        }
    }
    if (rom_count && !open_session(&app, argv[rom_arg], rom_count == 2 ? argv[rom_arg + 1] : NULL)) {
        if (app.fuji_link) {
            cpc_fuji_link_destroy(app.fuji_link);
        }
        return EXIT_FAILURE;
    }

    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec format = {
            .freq = CPC_AUDIO_HZ,
            .format = AUDIO_F32SYS,
            .channels = 1,
            .samples = 512
        };
        app.audio = SDL_OpenAudioDevice(NULL, 0, &format, NULL, 0);
        if (app.audio) {
            SDL_PauseAudioDevice(app.audio, 0);
        }
    }
    if (!app.audio) {
        g_warning("CPC audio unavailable: %s", SDL_GetError());
    }

    AdwApplication *application = adw_application_new(
        "online.fujinet.go.amstrad.gnome", app.managed ? G_APPLICATION_DEFAULT_FLAGS : G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(application), 1, argv);
    g_clear_handle_id(&app.timer, g_source_remove);
    g_object_unref(application);
    if (app.controller) {
        SDL_GameControllerClose(app.controller);
    }
    if (app.audio) {
        SDL_CloseAudioDevice(app.audio);
    }
    SDL_Quit();
    app.closing = true;
    stop_runtime(&app);
    while (app.runtime) {
        g_main_context_iteration(NULL, TRUE);
    }
    if (app.fuji_link) {
        cpc_fuji_link_destroy(app.fuji_link);
    }
    if (app.session) {
        cpc_session_destroy(app.session);
    }
    return status;
}
