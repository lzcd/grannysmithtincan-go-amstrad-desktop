#define _POSIX_C_SOURCE 200809L

#include "cpcsession.h"
#include "cpcdebug.h"
#include "fuji_link.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    BOOT_FRAMES = 100,
    TICKS_PER_FRAME = 80000,
    TONE_FRAMES = 50
};

typedef struct {
    size_t count;
    float min;
    float max;
} audio_probe;

static void probe_audio(const float *samples, size_t count, void *user_data)
{
    audio_probe *probe = user_data;
    if (probe->count == 0 && count > 0) {
        probe->min = samples[0];
        probe->max = samples[0];
    }
    probe->count += count;
    for (size_t i = 0; i < count; ++i) {
        if (samples[i] < probe->min) {
            probe->min = samples[i];
        }
        if (samples[i] > probe->max) {
            probe->max = samples[i];
        }
    }
}

static bool run_frames(cpc_session *session, int frames)
{
    for (int i = 0; i < frames; ++i) {
        if (cpc_session_run_frame(session) != TICKS_PER_FRAME) {
            fputs("Unexpected CPC CPU tick count\n", stderr);
            return false;
        }
    }
    return true;
}

static bool type_command(cpc_session *session, const char *command)
{
    for (const char *key = command; *key; ++key) {
        cpc_session_key(session, (unsigned char)*key, true);
        if (!run_frames(session, 3)) {
            return false;
        }
        cpc_session_key(session, (unsigned char)*key, false);
        if (!run_frames(session, 3)) {
            return false;
        }
    }
    return true;
}

static bool capture(cpc_session *session, const char *directory, const char *name)
{
    cpc_frame frame = cpc_session_frame(session);
    size_t length = strlen(directory) + strlen(name) + 6;
    char *path = malloc(length);
    uint8_t *row = malloc((size_t)frame.width * 3);
    bool ok = false;

    if (path && row) {
        snprintf(path, length, "%s/%s.ppm", directory, name);
        FILE *file = fopen(path, "wb");
        if (file) {
            ok = fprintf(file, "P6\n%d %d\n255\n", frame.width, frame.height) > 0;
            for (int y = 0; ok && y < frame.height; ++y) {
                const uint8_t *pixels = frame.rgba + y * frame.stride;
                for (int x = 0; x < frame.width; ++x) {
                    memcpy(row + x * 3, pixels + x * 4, 3);
                }
                ok = fwrite(row, 3, (size_t)frame.width, file) == (size_t)frame.width;
            }
            ok = fclose(file) == 0 && ok;
        }
    }

    if (!ok) {
        fprintf(stderr, "Could not write %s/%s.ppm\n", directory, name);
    }
    free(row);
    free(path);
    return ok;
}

static bool border_changed(cpc_frame frame, const uint8_t *boot_pixels)
{
    int changed = 0;
    // Check the left and right margins, where the BASIC text cursor cannot blink.
    for (int y = 0; y < frame.height; ++y) {
        size_t offset = (size_t)y * frame.stride;
        size_t right = offset + (size_t)(frame.width - 16) * 4;
        if (memcmp(boot_pixels + offset, frame.rgba + offset, 16 * 4) != 0 ||
            memcmp(boot_pixels + right, frame.rgba + right, 16 * 4) != 0) {
            ++changed;
        }
    }
    return changed > frame.height / 2;
}

static bool check_debugger(cpc_session *session)
{
    cpc_session_debug_breakpoint(session, 0, true);
    cpc_session_reset(session);
    cpc_session_run_frame(session);
    cpc_debug_state first = cpc_session_debug_state(session);
    char instruction[32];
    cpc_session_debug_disassemble(session, first.pc, instruction, sizeof(instruction));
    if (!first.paused || first.pc != 0 || !instruction[0] || cpc_session_run_frame(session)) {
        fputs("The debugger did not pause at the reset breakpoint\n", stderr);
        return false;
    }
    cpc_session_debug_step(session);
    cpc_debug_state next = cpc_session_debug_state(session);
    if (!next.paused || next.pc == first.pc) {
        fputs("The debugger did not step to the next instruction\n", stderr);
        return false;
    }
    cpc_session_debug_breakpoint(session, 0, false);
    cpc_session_debug_resume(session);
    if (cpc_session_debug_state(session).paused || !run_frames(session, BOOT_FRAMES)) {
        return false;
    }
    cpc_session_debug_pause(session);
    cpc_session_run_frame(session);
    return cpc_session_debug_state(session).paused;
}

static bool check_boot(cpc_session *session, audio_probe *probe, const char *directory)
{
    if (!run_frames(session, BOOT_FRAMES) || !capture(session, directory, "boot")) {
        return false;
    }
    cpc_frame frame = cpc_session_frame(session);
    size_t size = (size_t)frame.height * frame.stride;
    uint8_t *boot_pixels = malloc(size);
    if (!boot_pixels) {
        fputs("Could not save the boot frame\n", stderr);
        return false;
    }
    memcpy(boot_pixels, frame.rgba, size);

    // Change the border visibly and play a two-second tone through BASIC.
    bool ok = type_command(session, "border 6:sound 1,100,200,7\r");
    *probe = (audio_probe){0};
    ok = ok && run_frames(session, TONE_FRAMES) && capture(session, directory, "command");
    if (ok && !border_changed(cpc_session_frame(session), boot_pixels)) {
        fputs("BASIC did not change the border color\n", stderr);
        ok = false;
    }
    if (ok && (probe->count < CPC_AUDIO_HZ - 512 || probe->count > CPC_AUDIO_HZ + 512 ||
               probe->max - probe->min < 0.005f)) {
        fputs("Expected audio samples with an audible tone\n", stderr);
        ok = false;
    }

    if (ok) {
        cpc_session_reset(session);
        ok = run_frames(session, BOOT_FRAMES) && capture(session, directory, "reset");
        if (ok && memcmp(boot_pixels, cpc_session_frame(session).rgba, size) != 0) {
            fputs("Cold reset did not reproduce the boot frame\n", stderr);
            ok = false;
        }
    }

    free(boot_pixels);
    return ok && check_debugger(session);
}

static bool fuji_request(cpc_session *session, cpc_fuji_request *request)
{
    if (!type_command(session, "|fuji\r")) {
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        if (cpc_session_fuji_take(session, request)) {
            if (request->size == 2 && request->data[0] == CPC_FUJI_PING &&
                request->data[1] == CPC_FUJI_PING_DATA) {
                return true;
            }
            fputs("Unexpected payload from |FUJI\n", stderr);
            return false;
        }
        if (!run_frames(session, 1)) {
            return false;
        }
    }
    fputs("No mailbox request from |FUJI\n", stderr);
    return false;
}

static bool check_fuji(cpc_session *session, const char *directory)
{
    static const uint8_t reply[] = "PONG";
    cpc_fuji_request in_flight, retry;
    cpc_session_enable_fuji(session);
    if (!run_frames(session, BOOT_FRAMES) || !fuji_request(session, &in_flight)) {
        capture(session, directory, "fuji-reply");
        return false;
    }
    if (!cpc_session_fuji_reply(session, in_flight.ticket, reply, sizeof(reply) - 1) ||
        !run_frames(session, 10) || !capture(session, directory, "fuji-reply") ||
        !fuji_request(session, &in_flight) || !run_frames(session, 35) ||
        !capture(session, directory, "fuji-timeout") || !fuji_request(session, &retry)) {
        return false;
    }
    if (cpc_session_fuji_reply(session, in_flight.ticket, reply, sizeof(reply) - 1)) {
        fputs("A timed-out reply was accepted during a retry\n", stderr);
        return false;
    }

    cpc_session_reset(session);
    if (!run_frames(session, BOOT_FRAMES) || !fuji_request(session, &in_flight)) {
        return false;
    }
    if (cpc_session_fuji_reply(session, retry.ticket, reply, sizeof(reply) - 1)) {
        fputs("A reply from before reset was accepted\n", stderr);
        return false;
    }
    return cpc_session_fuji_reply(session, in_flight.ticket, reply, sizeof(reply) - 1) &&
           run_frames(session, 10) && capture(session, directory, "fuji-reset");
}

static bool type_target(cpc_session *session, const char *target)
{
    char input[CPC_FUJI_CAPACITY + 1];
    size_t size = strlen(target);
    if (size >= CPC_FUJI_CAPACITY) {
        fputs("Expected a TNFS host and optional path of at most 63 characters\n", stderr);
        return false;
    }
    memcpy(input, target, size);
    memcpy(input + size, "\r", 2);
    return type_command(session, input);
}

static void wait_live_frame(void)
{
    struct timespec delay = { .tv_nsec = CPC_FRAME_US * 1000 };
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

static bool check_live(cpc_session *session, cpc_fuji_link *link, const char *target,
                       const char *directory, bool check_prompt)
{
    cpc_session_enable_live_fuji(session);
    // Cold reset into CONFIG also exercises the normal desktop power-on sequence.
    cpc_session_reset_to_config(session);
    if (!run_frames(session, 150) ||
        (check_prompt && !type_command(session, "\x03|fuji\rx\x01")) ||
        !type_target(session, target)) {
        return false;
    }

    for (int frame = 0; frame < 1500; ++frame) {
        cpc_fuji_link_service(link, session);
        if (cpc_fuji_link_listing(link) != CPC_FUJI_LISTING_PENDING) {
            break;
        }
        if (!run_frames(session, 1)) {
            return false;
        }
        wait_live_frame();
    }
    if (!run_frames(session, 10) || !capture(session, directory, "fuji-directory")) {
        return false;
    }
    if (cpc_fuji_link_listing(link) != CPC_FUJI_LISTING_DONE) {
        fputs("The FujiNet directory did not reach end of listing\n", stderr);
        return false;
    }
    return true;
}

static bool check_run(cpc_session *session, cpc_fuji_link *link, const char *target,
                      const char *directory)
{
    if (!*target) {
        fputs("Expected a TNFS path to a program\n", stderr);
        return false;
    }
    cpc_session_enable_live_fuji(session);
    if (!run_frames(session, BOOT_FRAMES)) {
        return false;
    }
    cpc_frame frame = cpc_session_frame(session);
    size_t size = (size_t)frame.height * frame.stride;
    uint8_t *pixels = malloc(size);
    if (!pixels) {
        return false;
    }
    memcpy(pixels, frame.rgba, size);

    bool ok = type_command(session, "|fujirun\r") && type_target(session, target);
    for (int i = 0; ok && i < 1500 && !border_changed(cpc_session_frame(session), pixels); ++i) {
        cpc_fuji_link_service(link, session);
        ok = run_frames(session, 1);
        wait_live_frame();
    }
    ok = run_frames(session, 10) && capture(session, directory, "fuji-program") && ok;
    if (ok && !border_changed(cpc_session_frame(session), pixels)) {
        fputs("The loaded CPC program did not change the border\n", stderr);
        ok = false;
    }
    // Returning from the program must leave BASIC ready for another command.
    if (ok) {
        memcpy(pixels, cpc_session_frame(session).rgba, size);
        ok = type_command(session, "border 0\r") && run_frames(session, 10) &&
             border_changed(cpc_session_frame(session), pixels);
        if (!ok) {
            fputs("BASIC did not accept a command after the loaded program returned\n", stderr);
        }
    }
    free(pixels);
    return ok;
}

int main(int argc, char **argv)
{
    bool fuji = argc > 1 && strcmp(argv[1], "--fuji") == 0;
    bool live = argc > 1 && strcmp(argv[1], "--fuji-live") == 0;
    bool fixture = argc > 1 && strcmp(argv[1], "--fuji-fixture") == 0;
    bool run = argc > 1 && strcmp(argv[1], "--fuji-run") == 0;
    bool run_fixture = argc > 1 && strcmp(argv[1], "--fuji-run-fixture") == 0;
    int output_arg = live || fixture || run || run_fixture ? 4 : fuji ? 2 : 1;
    int rom_arg = output_arg + 1;
    int rom_count = argc - rom_arg;
    if (rom_count != 1 && rom_count != 2) {
        fprintf(stderr, "Usage: %s [--fuji] OUTPUT_DIR CPC464_ROM\n"
                        "       %s [--fuji] OUTPUT_DIR OS_ROM BASIC_ROM\n"
                        "       %s --fuji-live BOIP_PORT HOST[/PATH]|@SLOT|'' OUTPUT_DIR CPC464_ROM [BASIC_ROM]\n"
                        "       %s --fuji-run BOIP_PORT HOST/PATH/FILE OUTPUT_DIR CPC464_ROM [BASIC_ROM]\n",
                argv[0], argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    char error[512];
    audio_probe probe = {0};
    cpc_session *session = cpc_session_create(argv[rom_arg], rom_count == 2 ? argv[rom_arg + 1] : NULL,
                                              probe_audio, &probe, error, sizeof(error));
    if (!session) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }

    cpc_fuji_link *link = NULL;
    if (live || run) {
        link = cpc_fuji_link_create(argv[2]);
    } else if (fixture || run_fixture) {
        char *end;
        long fd = strtol(argv[2], &end, 10);
        if (end != argv[2] && !*end && fd >= 0 && fd <= INT_MAX) {
            link = cpc_fuji_link_from_fd((int)fd);
        }
    }
    bool ok;
    if (live || fixture || run || run_fixture) {
        if (!link) {
            fputs("Could not start FujiNet link\n", stderr);
        }
        ok = link && ((run || run_fixture) ? check_run(session, link, argv[3], argv[output_arg]) :
                      check_live(session, link, argv[3], argv[output_arg], fixture));
    } else {
        ok = fuji ? check_fuji(session, argv[output_arg]) : check_boot(session, &probe, argv[output_arg]);
    }
    if (link) {
        cpc_fuji_link_destroy(link);
    }
    cpc_session_destroy(session);
    if (ok) {
        puts(run || run_fixture ? "CPC 464 ran the TNFS program and returned to BASIC" :
             live || fixture ? "CPC 464 CONFIG reached the end of the TNFS directory" :
             fuji ? "CPC 464 |FUJI request, timeout, retry, and reset checks passed" :
                    "CPC 464 keyboard, border, audio, reset, and 50 Hz cadence checks passed");
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
