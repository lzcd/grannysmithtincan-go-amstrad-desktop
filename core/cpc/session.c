#include "cpcsession.h"
#include "cpcdebug.h"
#include "fuji_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cpc_expansion_io(void *machine, uint64_t *pins);
static const uint8_t *cpc_expansion_upper_rom(const void *machine, uint8_t slot,
                                               const uint8_t *fallback);

#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/ay38910.h"
#include "chips/i8255.h"
#include "chips/mc6845.h"
#include "chips/am40010.h"
#include "chips/upd765.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "chips/fdd.h"
#include "chips/fdd_cpc.h"
#include "cpc.h"
#define CHIPS_UTIL_IMPL
#include "util/z80dasm.h"

enum {
    ROM_SIZE = 16 * 1024,
    FRAME_WIDTH = AM40010_DISPLAY_WIDTH,
    FRAME_HEIGHT = 2 * AM40010_DISPLAY_HEIGHT,
    FRAME_STRIDE = FRAME_WIDTH * 4,
    BASIC_BOOT_FRAMES = 100,
    KEY_FRAMES = 3
};

struct cpc_session {
    // The staged core passes this first member back to the expansion hooks.
    cpc_t machine;
    cpc_fuji_card fuji;
    uint8_t roms[2 * ROM_SIZE];
    uint8_t pixels[FRAME_HEIGHT * FRAME_STRIDE];
    cpc_audio_fn audio;
    void *audio_data;
    unsigned config_boot_frame;
    bool config_boot;
    bool paused;
    bool pause_requested;
    uint16_t instruction;
    size_t breakpoint_count;
    uint8_t breakpoints[65536 / 8];
};

static bool cpc_expansion_io(void *machine, uint64_t *pins)
{
    cpc_session *session = machine;
    if (!(*pins & (Z80_RD | Z80_WR))) {
        return false;
    }

    bool write = (*pins & Z80_WR) != 0;
    uint8_t value = Z80_GET_DATA(*pins);
    if (!cpc_fuji_card_io(&session->fuji, Z80_GET_ADDR(*pins), write, &value)) {
        return false;
    }
    if (!write) {
        Z80_SET_DATA(*pins, value);
    }
    return true;
}

static const uint8_t *cpc_expansion_upper_rom(const void *machine, uint8_t slot,
                                               const uint8_t *fallback)
{
    const cpc_session *session = machine;
    return cpc_fuji_card_rom(&session->fuji, slot, fallback);
}

static bool read_rom(const char *path, uint8_t *rom, size_t size,
                     char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(error, error_size, "Could not open ROM '%s'", path);
        return false;
    }

    bool valid = fread(rom, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!valid) {
        snprintf(error, error_size, "Expected a readable %zu KiB ROM at '%s'",
                 size / 1024, path);
    }
    return valid;
}

static void forward_audio(const float *samples, int count, void *user_data)
{
    cpc_session *session = user_data;
    if (session->audio) {
        session->audio(samples, (size_t)count, session->audio_data);
    }
}

static void debug_tick(void *user_data, uint64_t pins)
{
    cpc_session *session = user_data;
    if (z80_opdone(&session->machine.cpu)) {
        uint16_t address = Z80_GET_ADDR(pins);
        session->instruction = address;
        if (session->pause_requested ||
            (session->breakpoints[address / 8] & (1u << (address % 8)))) {
            session->pause_requested = false;
            session->paused = true;
        }
    }
}

static void update_debug_callback(cpc_session *session)
{
    session->machine.debug.callback.func =
        session->pause_requested || session->breakpoint_count ? debug_tick : NULL;
}

static void boot(cpc_session *session)
{
    cpc_init(&session->machine, &(cpc_desc_t){
        .type = CPC_TYPE_464,
        .debug = { .callback = { .user_data = session }, .stopped = &session->paused },
        .audio = {
            .callback = { .func = forward_audio, .user_data = session },
            .sample_rate = CPC_AUDIO_HZ
        },
        .roms.cpc464 = {
            .os = { .ptr = session->roms, .size = ROM_SIZE },
            .basic = { .ptr = session->roms + ROM_SIZE, .size = ROM_SIZE }
        }
    });
    update_debug_callback(session);
}

cpc_session *cpc_session_create(const char *rom_path, const char *basic_path,
                               cpc_audio_fn audio, void *user_data,
                               char *error, size_t error_size)
{
    // chips gives the CPC framebuffer an extended alignment.
    cpc_session *session = aligned_alloc(_Alignof(cpc_session), sizeof(*session));
    if (!session) {
        snprintf(error, error_size, "Could not allocate a CPC session");
        return NULL;
    }

    size_t size = basic_path ? ROM_SIZE : sizeof(session->roms);
    if (!read_rom(rom_path, session->roms, size, error, error_size) ||
        (basic_path && !read_rom(basic_path, session->roms + ROM_SIZE, ROM_SIZE,
                                 error, error_size))) {
        free(session);
        return NULL;
    }

    session->audio = audio;
    session->audio_data = user_data;
    session->fuji = (cpc_fuji_card){0};
    session->breakpoint_count = 0;
    memset(session->breakpoints, 0, sizeof(session->breakpoints));
    session->paused = false;
    session->pause_requested = false;
    session->instruction = 0;
    session->config_boot = false;
    session->config_boot_frame = 0;
    memset(session->pixels, 0, sizeof(session->pixels));
    boot(session);
    return session;
}

void cpc_session_destroy(cpc_session *session)
{
    cpc_discard(&session->machine);
    free(session);
}

void cpc_session_reset(cpc_session *session)
{
    cpc_discard(&session->machine);
    cpc_fuji_card_reset(&session->fuji);
    session->config_boot = false;
    session->paused = false;
    session->pause_requested = false;
    session->instruction = 0;
    memset(session->pixels, 0, sizeof(session->pixels));
    boot(session);
}

void cpc_session_reset_to_config(cpc_session *session)
{
    cpc_session_reset(session);
    session->config_boot = session->fuji.enabled && session->fuji.live;
    session->config_boot_frame = 0;
}

static void config_boot_key(cpc_session *session)
{
    static const char command[] = "|fuji\r";
    unsigned frame = session->config_boot_frame++;
    if (session->config_boot_frame == BASIC_BOOT_FRAMES + (sizeof(command) - 1) * 2 * KEY_FRAMES) {
        session->config_boot = false;
    }
    if (frame >= BASIC_BOOT_FRAMES) {
        unsigned key_frame = frame - BASIC_BOOT_FRAMES;
        int key = command[key_frame / (2 * KEY_FRAMES)];
        switch (key_frame % (2 * KEY_FRAMES)) {
        case 0: cpc_key_down(&session->machine, key); break;
        case KEY_FRAMES: cpc_key_up(&session->machine, key); break;
        }
    }
}

static void render(cpc_session *session)
{
    chips_display_info_t display = cpc_display_info(&session->machine);
    const uint8_t *indices = display.frame.buffer.ptr;
    const uint32_t *palette = display.palette.ptr;
    for (int y = 0; y < display.screen.height; ++y) {
        const uint8_t *source = indices + (display.screen.y + y) * display.frame.dim.width
                                       + display.screen.x;
        uint8_t *line = session->pixels + 2 * y * FRAME_STRIDE;
        uint8_t *target = line;
        for (int x = 0; x < display.screen.width; ++x) {
            uint32_t color = palette[source[x]];
            *target++ = (uint8_t)color;
            *target++ = (uint8_t)(color >> 8);
            *target++ = (uint8_t)(color >> 16);
            *target++ = 255;
        }
        memcpy(line + FRAME_STRIDE, line, FRAME_STRIDE);
    }
}

uint32_t cpc_session_run_frame(cpc_session *session)
{
    if (session->paused) {
        return 0;
    }
    if (session->config_boot) {
        config_boot_key(session);
    }
    cpc_fuji_card_advance(&session->fuji, CPC_FRAME_US);
    uint32_t ticks = cpc_exec(&session->machine, CPC_FRAME_US);
    render(session);
    return ticks;
}

void cpc_session_key(cpc_session *session, int key, bool pressed)
{
    if (pressed) {
        cpc_key_down(&session->machine, key);
    } else {
        cpc_key_up(&session->machine, key);
    }
}

void cpc_session_joystick(cpc_session *session, uint8_t mask)
{
    cpc_joystick(&session->machine, mask);
}

bool cpc_session_load_snapshot(cpc_session *session, const void *data, size_t size)
{
    const _cpc_sna_header *header = data;
    if (!data || size != sizeof(*header) + 65536 || memcmp(header->magic, "MV - SNA", 8) ||
        header->version < 1 || header->version > 2 || header->dump_size_l != 64 ||
        header->dump_size_h || header->IM > 2 || header->crtc_selected >= 32 ||
        header->psg_selected >= 16) {
        return false;
    }
    cpc_session_reset(session);
    if (!cpc_quickload(&session->machine, (chips_range_t){ (void *)data, size }, false)) {
        return false;
    }
    session->machine.pins = z80_prefetch(&session->machine.cpu, session->machine.cpu.pc);
    return true;
}

cpc_debug_state cpc_session_debug_state(const cpc_session *session)
{
    const z80_t *cpu = &session->machine.cpu;
    return (cpc_debug_state){
        .pc = session->paused ? session->instruction : cpu->pc,
        .sp = cpu->sp, .af = cpu->af, .bc = cpu->bc, .de = cpu->de,
        .hl = cpu->hl, .ix = cpu->ix, .iy = cpu->iy, .paused = session->paused
    };
}

void cpc_session_debug_pause(cpc_session *session)
{
    session->pause_requested = !session->paused;
    update_debug_callback(session);
}

void cpc_session_debug_resume(cpc_session *session)
{
    session->paused = false;
    session->pause_requested = false;
    update_debug_callback(session);
}

void cpc_session_debug_step(cpc_session *session)
{
    if (session->paused) {
        cpc_session_debug_resume(session);
        cpc_session_debug_pause(session);
        cpc_exec(&session->machine, 1000);
        render(session);
    }
}

void cpc_session_debug_breakpoint(cpc_session *session, uint16_t address, bool enabled)
{
    uint8_t *byte = &session->breakpoints[address / 8];
    uint8_t mask = 1u << (address % 8);
    if (enabled && !(*byte & mask)) {
        *byte |= mask;
        ++session->breakpoint_count;
    } else if (!enabled && (*byte & mask)) {
        *byte &= ~mask;
        --session->breakpoint_count;
    }
    update_debug_callback(session);
}

uint8_t cpc_session_debug_read(const cpc_session *session, uint16_t address)
{
    return mem_rd((mem_t *)&session->machine.mem, address);
}

typedef struct {
    const cpc_session *session;
    uint16_t address;
    char *text;
    size_t left;
} disassembly;

static uint8_t disassembly_byte(void *data)
{
    disassembly *line = data;
    return cpc_session_debug_read(line->session, line->address++);
}

static void disassembly_char(char ch, void *data)
{
    disassembly *line = data;
    if (line->left > 1) {
        *line->text++ = ch;
        --line->left;
    }
}

uint16_t cpc_session_debug_disassemble(const cpc_session *session, uint16_t address,
                                       char *text, size_t size)
{
    disassembly line = { session, address, text, size };
    uint16_t next = z80dasm_op(address, disassembly_byte, disassembly_char, &line);
    if (size) {
        *line.text = 0;
    }
    return next;
}

cpc_frame cpc_session_frame(const cpc_session *session)
{
    return (cpc_frame){
        .rgba = session->pixels,
        .width = FRAME_WIDTH,
        .height = FRAME_HEIGHT,
        .stride = FRAME_STRIDE
    };
}

void cpc_session_enable_fuji(cpc_session *session)
{
    session->fuji.enabled = true;
    session->fuji.live = false;
    cpc_session_reset(session);
}

void cpc_session_enable_live_fuji(cpc_session *session)
{
    session->fuji.enabled = true;
    session->fuji.live = true;
    cpc_session_reset(session);
}

void cpc_session_disable_fuji(cpc_session *session)
{
    session->fuji.enabled = false;
    session->fuji.live = false;
    cpc_session_reset(session);
}

bool cpc_session_fuji_take(cpc_session *session, cpc_fuji_request *request)
{
    return cpc_fuji_card_take(&session->fuji, request);
}

bool cpc_session_fuji_pending(const cpc_session *session, uint64_t ticket)
{
    return session->fuji.status == FUJI_BUSY && session->fuji.ticket == ticket;
}

bool cpc_session_fuji_reply(cpc_session *session, uint64_t ticket,
                            const uint8_t *data, size_t size)
{
    return cpc_fuji_card_reply(&session->fuji, ticket, data, size);
}
