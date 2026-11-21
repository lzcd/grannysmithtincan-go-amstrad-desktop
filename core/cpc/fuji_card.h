#ifndef FUJI_CARD_H
#define FUJI_CARD_H

#include "cpcfuji.h"

enum {
    FUJI_ROM_SLOT = 5,
    FUJI_PORT_ID = 0xfd60,
    FUJI_PORT_STATUS = 0xfd61,
    FUJI_PORT_SIZE = 0xfd62,
    FUJI_PORT_OFFSET = 0xfd63,
    FUJI_PORT_DATA = 0xfd64,
    FUJI_PORT_ERROR = 0xfd65,
    FUJI_ID = 0xf1,
    FUJI_LIVE_ID = 0xf2,
    FUJI_CLEAR = 0,
    FUJI_SUBMIT = 1,
    FUJI_TIMEOUT_US = 500000,
    FUJI_LIVE_TIMEOUT_US = 5000000
};

typedef enum {
    FUJI_IDLE = 0,
    FUJI_BUSY = 1,
    FUJI_READY = 2,
    FUJI_ERROR = 3
} fuji_status;

typedef enum {
    FUJI_ERR_NONE = 0,
    FUJI_ERR_REQUEST_SIZE = 1,
    FUJI_ERR_TIMEOUT = 2,
    FUJI_ERR_REPLY_SIZE = 3
} fuji_error;

typedef struct {
    bool enabled;
    bool live;
    bool taken;
    uint64_t ticket;
    uint32_t time_left_us;
    fuji_status status;
    fuji_error error;
    uint8_t size;
    uint8_t offset;
    uint8_t data[CPC_FUJI_CAPACITY];
} cpc_fuji_card;

void cpc_fuji_card_reset(cpc_fuji_card *card);
const uint8_t *cpc_fuji_card_rom(const cpc_fuji_card *card, uint8_t slot,
                                const uint8_t *fallback);
bool cpc_fuji_card_io(cpc_fuji_card *card, uint16_t port, bool write, uint8_t *value);
void cpc_fuji_card_advance(cpc_fuji_card *card, uint32_t us);
bool cpc_fuji_card_take(cpc_fuji_card *card, cpc_fuji_request *request);
bool cpc_fuji_card_reply(cpc_fuji_card *card, uint64_t ticket,
                         const uint8_t *data, size_t size);

#endif
