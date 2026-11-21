#ifndef CPCSESSION_H
#define CPCSESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpcfuji.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CPC_FRAME_US = 20000,
    CPC_AUDIO_HZ = 44100
};

typedef struct cpc_session cpc_session;
typedef void (*cpc_audio_fn)(const float *samples, size_t count, void *user_data);

typedef struct {
    const uint8_t *rgba;
    int width;
    int height;
    size_t stride;
} cpc_frame;

// With basic_path set, read two 16 KiB ROMs. Otherwise read one 32 KiB OS+BASIC image.
cpc_session *cpc_session_create(const char *rom_path, const char *basic_path,
                               cpc_audio_fn audio, void *user_data,
                               char *error, size_t error_size);
void cpc_session_destroy(cpc_session *session);
void cpc_session_reset(cpc_session *session);
void cpc_session_reset_to_config(cpc_session *session);
uint32_t cpc_session_run_frame(cpc_session *session);
// Printable ASCII and the CPC key codes for special keys.
void cpc_session_key(cpc_session *session, int key, bool pressed);
enum {
    CPC_JOY_UP = 1 << 0, CPC_JOY_DOWN = 1 << 1,
    CPC_JOY_LEFT = 1 << 2, CPC_JOY_RIGHT = 1 << 3,
    CPC_JOY_FIRE = 1 << 4
};
void cpc_session_joystick(cpc_session *session, uint8_t mask);
bool cpc_session_load_snapshot(cpc_session *session, const void *data, size_t size);
// Pixel storage belongs to the session and is valid until the next frame or reset.
// Each CPC scanline is doubled for display at square pixels.
cpc_frame cpc_session_frame(const cpc_session *session);

// Attach the prototype card and cold boot so the firmware discovers its ROM.
void cpc_session_enable_fuji(cpc_session *session);
void cpc_session_enable_live_fuji(cpc_session *session);
void cpc_session_disable_fuji(cpc_session *session);
// The session owner takes a copy of each request and publishes replies on the same thread.
bool cpc_session_fuji_take(cpc_session *session, cpc_fuji_request *request);
bool cpc_session_fuji_pending(const cpc_session *session, uint64_t ticket);
// A matching ticket is consumed even if its invalid reply length puts the card in error.
bool cpc_session_fuji_reply(cpc_session *session, uint64_t ticket,
                            const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
