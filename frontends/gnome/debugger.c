#include "debugger.h"

#include <string.h>

static bool read_address(GtkEntry *entry, uint16_t *address)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (*text == '&') {
        ++text;
    }
    guint64 value;
    if (!g_ascii_string_to_unsigned(text, 16, 0, G_MAXUINT16, &value, NULL)) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return false;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    *address = (uint16_t)value;
    return true;
}

void cpc_debugger_refresh(cpc_debugger *debugger)
{
    if (!debugger->window || !gtk_widget_get_visible(GTK_WIDGET(debugger->window))) {
        return;
    }
    if (!debugger->session) {
        gtk_text_buffer_set_text(debugger->screen, "Import CPC firmware to start the debugger.", -1);
        return;
    }
    cpc_debug_state cpu = cpc_session_debug_state(debugger->session);
    g_autoptr(GString) text = g_string_new(cpu.paused ? "Paused\n" : "Running\n");
    g_string_append_printf(text, "PC %04X   SP %04X   AF %04X   BC %04X\n"
                           "DE %04X   HL %04X   IX %04X   IY %04X\n\n",
                           cpu.pc, cpu.sp, cpu.af, cpu.bc, cpu.de, cpu.hl, cpu.ix, cpu.iy);
    uint16_t address = cpu.pc;
    for (int line = 0; line < 9; ++line) {
        char mnemonic[64];
        uint16_t next = cpc_session_debug_disassemble(debugger->session, address,
                                                      mnemonic, sizeof(mnemonic));
        g_string_append_printf(text, "%s %04X  %s\n", line == 0 ? ">" : " ", address, mnemonic);
        address = next;
    }
    if (read_address(debugger->memory, &address)) {
        g_string_append(text, "\nMemory\n");
        for (int row = 0; row < 8; ++row) {
            g_string_append_printf(text, "%04X ", address);
            for (int col = 0; col < 16; ++col) {
                g_string_append_printf(text, " %02X",
                                       cpc_session_debug_read(debugger->session, address++));
            }
            g_string_append_c(text, '\n');
        }
    }
    gtk_text_buffer_set_text(debugger->screen, text->str, -1);
}

static void pause_cpu(GtkButton *button, gpointer data)
{
    (void)button;
    cpc_debugger *debugger = data;
    if (debugger->session) {
        cpc_session_debug_pause(debugger->session);
    }
}

static void run_cpu(GtkButton *button, gpointer data)
{
    (void)button;
    cpc_debugger *debugger = data;
    if (debugger->session) {
        cpc_session_debug_resume(debugger->session);
    }
}

static void step_cpu(GtkButton *button, gpointer data)
{
    (void)button;
    cpc_debugger *debugger = data;
    if (debugger->session) {
        cpc_session_debug_step(debugger->session);
        cpc_debugger_refresh(debugger);
    }
}

static void toggle_breakpoint(GtkButton *button, gpointer data)
{
    (void)button;
    cpc_debugger *debugger = data;
    uint16_t address;
    if (!read_address(debugger->breakpoint, &address)) {
        return;
    }
    guint index = 0;
    while (index < debugger->breakpoints->len &&
           g_array_index(debugger->breakpoints, uint16_t, index) != address) {
        ++index;
    }
    bool enable = index == debugger->breakpoints->len;
    if (enable) {
        g_array_append_val(debugger->breakpoints, address);
    } else {
        g_array_remove_index(debugger->breakpoints, index);
    }
    if (debugger->session) {
        cpc_session_debug_breakpoint(debugger->session, address, enable);
    }
    g_autoptr(GString) label = g_string_new("");
    for (guint i = 0; i < debugger->breakpoints->len; ++i) {
        g_string_append_printf(label, " %04X",
                               g_array_index(debugger->breakpoints, uint16_t, i));
    }
    gtk_label_set_text(debugger->breakpoints_label, label->str);
}

static void add_button(GtkBox *row, const char *label, GCallback callback, cpc_debugger *debugger)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_signal_connect(button, "clicked", callback, debugger);
    gtk_box_append(row, button);
}

void cpc_debugger_show(cpc_debugger *debugger, GtkWindow *parent)
{
    if (!debugger->window) {
        debugger->breakpoints = g_array_new(FALSE, FALSE, sizeof(uint16_t));
        debugger->window = GTK_WINDOW(gtk_window_new());
        gtk_window_set_title(debugger->window, "Z80 Debugger");
        gtk_window_set_default_size(debugger->window, 650, 570);
        gtk_window_set_transient_for(debugger->window, parent);
        gtk_window_set_hide_on_close(debugger->window, TRUE);

        GtkBox *content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
        gtk_widget_set_margin_top(GTK_WIDGET(content), 12);
        gtk_widget_set_margin_bottom(GTK_WIDGET(content), 12);
        gtk_widget_set_margin_start(GTK_WIDGET(content), 12);
        gtk_widget_set_margin_end(GTK_WIDGET(content), 12);
        gtk_window_set_child(debugger->window, GTK_WIDGET(content));

        GtkBox *toolbar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
        gtk_box_append(content, GTK_WIDGET(toolbar));
        add_button(toolbar, "Pause", G_CALLBACK(pause_cpu), debugger);
        add_button(toolbar, "Step", G_CALLBACK(step_cpu), debugger);
        add_button(toolbar, "Continue", G_CALLBACK(run_cpu), debugger);
        GtkBox *addresses = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
        gtk_box_append(content, GTK_WIDGET(addresses));
        debugger->memory = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(debugger->memory), "0000");
        gtk_editable_set_width_chars(GTK_EDITABLE(debugger->memory), 6);
        gtk_box_append(addresses, gtk_label_new("Memory address"));
        gtk_box_append(addresses, GTK_WIDGET(debugger->memory));
        debugger->breakpoint = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_width_chars(GTK_EDITABLE(debugger->breakpoint), 6);
        gtk_entry_set_placeholder_text(debugger->breakpoint, "Hex");
        gtk_box_append(addresses, GTK_WIDGET(debugger->breakpoint));
        add_button(addresses, "Toggle Breakpoint", G_CALLBACK(toggle_breakpoint), debugger);
        debugger->breakpoints_label = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_wrap(debugger->breakpoints_label, TRUE);
        gtk_box_append(content, GTK_WIDGET(debugger->breakpoints_label));

        GtkWidget *scroller = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroller, TRUE);
        GtkWidget *text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
        debugger->screen = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), text);
        gtk_box_append(content, scroller);
    }
    gtk_window_present(debugger->window);
    cpc_debugger_refresh(debugger);
}

void cpc_debugger_set_session(cpc_debugger *debugger, cpc_session *session)
{
    debugger->session = session;
    if (session && debugger->breakpoints) {
        for (guint i = 0; i < debugger->breakpoints->len; ++i) {
            cpc_session_debug_breakpoint(session,
                g_array_index(debugger->breakpoints, uint16_t, i), true);
        }
    }
}

void cpc_debugger_clear(cpc_debugger *debugger)
{
    if (debugger->window) {
        gtk_window_destroy(debugger->window);
        g_array_unref(debugger->breakpoints);
    }
}
