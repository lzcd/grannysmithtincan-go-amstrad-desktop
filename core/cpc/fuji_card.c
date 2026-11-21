#include "fuji_card.h"

#include <string.h>

#include "fuji_rom.h"

void cpc_fuji_card_reset(cpc_fuji_card *card)
{
    *card = (cpc_fuji_card){
        .enabled = card->enabled, .live = card->live, .ticket = card->ticket
    };
}

const uint8_t *cpc_fuji_card_rom(const cpc_fuji_card *card, uint8_t slot,
                                const uint8_t *fallback)
{
    return card->enabled && slot == FUJI_ROM_SLOT ? cpc_fuji_rom : fallback;
}

static void set_error(cpc_fuji_card *card, fuji_error error)
{
    card->status = FUJI_ERROR;
    card->error = error;
    card->size = 0;
    card->offset = 0;
}

static void submit(cpc_fuji_card *card)
{
    if (card->status != FUJI_IDLE) {
        return;
    }
    if (card->size == 0 || card->size > sizeof(card->data)) {
        set_error(card, FUJI_ERR_REQUEST_SIZE);
        return;
    }
    ++card->ticket;
    card->time_left_us = card->live ? FUJI_LIVE_TIMEOUT_US : FUJI_TIMEOUT_US;
    card->status = FUJI_BUSY;
}

static void write_register(cpc_fuji_card *card, uint16_t port, uint8_t value)
{
    switch (port) {
    case FUJI_PORT_STATUS:
        if (value == FUJI_CLEAR) {
            cpc_fuji_card_reset(card);
        } else if (value == FUJI_SUBMIT) {
            submit(card);
        }
        break;
    case FUJI_PORT_SIZE:
        if (card->status == FUJI_IDLE) {
            card->size = value;
        }
        break;
    case FUJI_PORT_OFFSET:
        if (card->status == FUJI_IDLE || card->status == FUJI_READY) {
            card->offset = value;
        }
        break;
    case FUJI_PORT_DATA:
        if (card->status == FUJI_IDLE && card->offset < sizeof(card->data)) {
            card->data[card->offset] = value;
        }
        break;
    }
}

static uint8_t read_register(const cpc_fuji_card *card, uint16_t port)
{
    switch (port) {
    case FUJI_PORT_ID: return card->live ? FUJI_LIVE_ID : FUJI_ID;
    case FUJI_PORT_STATUS: return card->status;
    case FUJI_PORT_ERROR: return card->error;
    case FUJI_PORT_SIZE: return card->status == FUJI_READY ? card->size : 0;
    case FUJI_PORT_OFFSET: return card->offset;
    case FUJI_PORT_DATA:
        if (card->status == FUJI_READY && card->offset < card->size) {
            return card->data[card->offset];
        }
        break;
    }
    return 0xff;
}

bool cpc_fuji_card_io(cpc_fuji_card *card, uint16_t port, bool write, uint8_t *value)
{
    if (!card->enabled || port < FUJI_PORT_ID || port > FUJI_PORT_ERROR) {
        return false;
    }
    if (write) {
        write_register(card, port, *value);
    } else {
        *value = read_register(card, port);
    }
    return true;
}

void cpc_fuji_card_advance(cpc_fuji_card *card, uint32_t us)
{
    if (card->status == FUJI_BUSY) {
        if (us >= card->time_left_us) {
            set_error(card, FUJI_ERR_TIMEOUT);
        } else {
            card->time_left_us -= us;
        }
    }
}

bool cpc_fuji_card_take(cpc_fuji_card *card, cpc_fuji_request *request)
{
    if (card->status != FUJI_BUSY || card->taken) {
        return false;
    }
    *request = (cpc_fuji_request){ .ticket = card->ticket, .size = card->size };
    memcpy(request->data, card->data, request->size);
    card->taken = true;
    return true;
}

bool cpc_fuji_card_reply(cpc_fuji_card *card, uint64_t ticket,
                         const uint8_t *data, size_t size)
{
    if (card->status != FUJI_BUSY || !card->taken || card->ticket != ticket) {
        return false;
    }
    if (!data || size == 0 || size > sizeof(card->data)) {
        set_error(card, FUJI_ERR_REPLY_SIZE);
    } else {
        memcpy(card->data, data, size);
        card->size = (uint8_t)size;
        card->offset = 0;
        card->status = FUJI_READY;
    }
    return true;
}
