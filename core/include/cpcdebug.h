#ifndef CPCDEBUG_H
#define CPCDEBUG_H

#include "cpcsession.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t pc, sp, af, bc, de, hl, ix, iy;
    bool paused;
} cpc_debug_state;

cpc_debug_state cpc_session_debug_state(const cpc_session *session);
void cpc_session_debug_pause(cpc_session *session);
void cpc_session_debug_resume(cpc_session *session);
void cpc_session_debug_step(cpc_session *session);
void cpc_session_debug_breakpoint(cpc_session *session, uint16_t address, bool enabled);
uint8_t cpc_session_debug_read(const cpc_session *session, uint16_t address);
uint16_t cpc_session_debug_disassemble(const cpc_session *session, uint16_t address,
                                       char *text, size_t size);

#ifdef __cplusplus
}
#endif
#endif
