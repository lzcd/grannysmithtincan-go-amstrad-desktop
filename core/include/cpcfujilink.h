#ifndef CPCFUJILINK_H
#define CPCFUJILINK_H

#include "cpcsession.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpc_fuji_link cpc_fuji_link;

typedef enum {
    CPC_FUJI_REQUEST,
    CPC_FUJI_REPLY,
    CPC_FUJI_TIMEOUT
} cpc_fuji_event;

typedef void (*cpc_fuji_trace_fn)(cpc_fuji_event event, uint64_t ticket,
                                 const uint8_t *bytes, size_t size, void *user_data);

typedef enum {
    CPC_FUJI_LISTING_PENDING,
    CPC_FUJI_LISTING_DONE,
    CPC_FUJI_LISTING_FAILED
} cpc_fuji_listing;

// Connects to an RS232 FujiNet-PC listener on IPv4 loopback.
cpc_fuji_link *cpc_fuji_link_create(const char *port);
void cpc_fuji_link_destroy(cpc_fuji_link *link);

// Call from the session owner. Network I/O runs on the worker.
void cpc_fuji_link_service(cpc_fuji_link *link, cpc_session *session);
void cpc_fuji_link_set_trace(cpc_fuji_link *link, cpc_fuji_trace_fn trace, void *user_data);
void cpc_fuji_link_reset(cpc_fuji_link *link);
cpc_fuji_listing cpc_fuji_link_listing(const cpc_fuji_link *link);

#ifdef __cplusplus
}
#endif

#endif
