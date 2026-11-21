#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

GSubprocess *cpc_runtime_start(const char *dist, const char *data,
                               unsigned boip_port, unsigned web_port, GError **error)
{
    g_autofree char *binary = g_build_filename(dist, "fujinet", NULL);
    g_autofree char *config = g_build_filename(data, "fnconfig.ini", NULL);
    g_autofree char *sd = g_build_filename(data, "SD", NULL);
    g_autofree char *log = g_build_filename(data, "fujinet.log", NULL);
    g_autofree char *url = g_strdup_printf("http://127.0.0.1:%u", web_port);
    if (!g_file_test(binary, G_FILE_TEST_IS_EXECUTABLE)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "FujiNet-PC was not found at %s", binary);
        return NULL;
    }
    if (g_mkdir_with_parents(sd, 0700) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not create the SD folder: %s", g_strerror(errno));
        return NULL;
    }

    g_autoptr(GKeyFile) settings = g_key_file_new();
    g_autofree char *defaults = g_build_filename(dist, "fnconfig.ini", NULL);
    if (g_file_test(config, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file(settings, config, G_KEY_FILE_KEEP_COMMENTS, error)) {
            return NULL;
        }
    } else if (g_file_test(defaults, G_FILE_TEST_EXISTS) &&
               !g_key_file_load_from_file(settings, defaults, G_KEY_FILE_KEEP_COMMENTS, error)) {
        return NULL;
    }
    g_key_file_set_integer(settings, "BOIP", "enabled", 1);
    g_key_file_set_string(settings, "BOIP", "host", "127.0.0.1");
    g_key_file_set_integer(settings, "BOIP", "port", boip_port);
    if (!g_key_file_save_to_file(settings, config, error)) {
        return NULL;
    }

    int log_fd = g_open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Could not open the FujiNet log: %s", g_strerror(errno));
        return NULL;
    }
    int stderr_fd = dup(log_fd);
    if (stderr_fd < 0) {
        int failure = errno;
        close(log_fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(failure),
                    "Could not open the FujiNet log: %s", g_strerror(failure));
        return NULL;
    }
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_cwd(launcher, dist);
    g_subprocess_launcher_take_stdout_fd(launcher, log_fd);
    g_subprocess_launcher_take_stderr_fd(launcher, stderr_fd);
    return g_subprocess_launcher_spawn(launcher, error, binary, "-u", url, "-c", config, "-s", sd, NULL);
}
