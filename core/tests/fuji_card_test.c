#include "fuji_card.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            exit(EXIT_FAILURE);                                            \
        }                                                                  \
    } while (0)

static uint8_t read_port(cpc_fuji_card *card, uint16_t port)
{
    uint8_t value = 0;
    CHECK(cpc_fuji_card_io(card, port, false, &value));
    return value;
}

static void write_port(cpc_fuji_card *card, uint16_t port, uint8_t value)
{
    CHECK(cpc_fuji_card_io(card, port, true, &value));
}

static cpc_fuji_request ping(cpc_fuji_card *card)
{
    write_port(card, FUJI_PORT_STATUS, FUJI_CLEAR);
    write_port(card, FUJI_PORT_SIZE, 2);
    write_port(card, FUJI_PORT_OFFSET, 1);
    write_port(card, FUJI_PORT_DATA, CPC_FUJI_PING_DATA);
    write_port(card, FUJI_PORT_OFFSET, 0);
    write_port(card, FUJI_PORT_DATA, CPC_FUJI_PING);
    write_port(card, FUJI_PORT_STATUS, FUJI_SUBMIT);
    write_port(card, FUJI_PORT_STATUS, FUJI_SUBMIT);
    CHECK(read_port(card, FUJI_PORT_STATUS) == FUJI_BUSY);
    CHECK(read_port(card, FUJI_PORT_SIZE) == 0);
    CHECK(read_port(card, FUJI_PORT_DATA) == 0xff);
    write_port(card, FUJI_PORT_SIZE, 1);
    write_port(card, FUJI_PORT_DATA, 0xff);

    cpc_fuji_request request;
    CHECK(cpc_fuji_card_take(card, &request));
    CHECK(!cpc_fuji_card_take(card, &request));
    CHECK(request.size == 2);
    CHECK(request.data[0] == CPC_FUJI_PING && request.data[1] == CPC_FUJI_PING_DATA);
    return request;
}

static void check_decode(void)
{
    static const uint8_t basic[] = {0x80};
    cpc_fuji_card card = {0};
    uint8_t value = 0;
    CHECK(cpc_fuji_card_rom(&card, FUJI_ROM_SLOT, basic) == basic);
    CHECK(!cpc_fuji_card_io(&card, FUJI_PORT_ID, false, &value));

    card.enabled = true;
    CHECK(cpc_fuji_card_rom(&card, FUJI_ROM_SLOT, basic)[0] == 1);
    CHECK(cpc_fuji_card_rom(&card, 0, basic) == basic);
    CHECK(cpc_fuji_card_rom(&card, 7, basic) == basic);
    CHECK(read_port(&card, FUJI_PORT_ID) == FUJI_ID);
    CHECK(!cpc_fuji_card_io(&card, 0xfc60, false, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfe60, false, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfd66, false, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfa7e, true, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfb7f, false, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfc00, true, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfe00, true, &value));
    CHECK(!cpc_fuji_card_io(&card, 0xfd23, false, &value));
}

static void check_buffers(void)
{
    cpc_fuji_card card = { .enabled = true };
    cpc_fuji_request request;
    write_port(&card, FUJI_PORT_SIZE, CPC_FUJI_CAPACITY + 1);
    write_port(&card, FUJI_PORT_STATUS, FUJI_SUBMIT);
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_ERROR);
    CHECK(read_port(&card, FUJI_PORT_ERROR) == FUJI_ERR_REQUEST_SIZE);
    CHECK(!cpc_fuji_card_take(&card, &request));

    write_port(&card, FUJI_PORT_STATUS, FUJI_CLEAR);
    write_port(&card, FUJI_PORT_SIZE, CPC_FUJI_CAPACITY);
    write_port(&card, FUJI_PORT_OFFSET, CPC_FUJI_CAPACITY - 1);
    write_port(&card, FUJI_PORT_DATA, 0xba);
    write_port(&card, FUJI_PORT_OFFSET, CPC_FUJI_CAPACITY);
    write_port(&card, FUJI_PORT_DATA, 0xee);
    write_port(&card, FUJI_PORT_STATUS, FUJI_SUBMIT);
    CHECK(cpc_fuji_card_take(&card, &request));
    CHECK(request.size == CPC_FUJI_CAPACITY);
    CHECK(request.data[CPC_FUJI_CAPACITY - 1] == 0xba && request.data[0] == 0);
    CHECK(cpc_fuji_card_reply(&card, request.ticket, request.data, request.size + 1));
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_ERROR);
    CHECK(read_port(&card, FUJI_PORT_ERROR) == FUJI_ERR_REPLY_SIZE);

    request = ping(&card);
    static const uint8_t reply[] = "PONG";
    CHECK(cpc_fuji_card_reply(&card, request.ticket, reply, sizeof(reply) - 1));
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_READY);
    CHECK(read_port(&card, FUJI_PORT_SIZE) == sizeof(reply) - 1);
    CHECK(read_port(&card, FUJI_PORT_ERROR) == FUJI_ERR_NONE);
    for (uint8_t i = 0; i < sizeof(reply) - 1; ++i) {
        write_port(&card, FUJI_PORT_OFFSET, i);
        CHECK(read_port(&card, FUJI_PORT_DATA) == reply[i]);
        CHECK(read_port(&card, FUJI_PORT_DATA) == reply[i]);
    }
    write_port(&card, FUJI_PORT_OFFSET, sizeof(reply) - 1);
    CHECK(read_port(&card, FUJI_PORT_DATA) == 0xff);
    write_port(&card, FUJI_PORT_STATUS, FUJI_CLEAR);
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_IDLE);
}

static void check_stale_replies(void)
{
    cpc_fuji_card card = { .enabled = true };
    cpc_fuji_request timed_out = ping(&card);
    cpc_fuji_card_advance(&card, FUJI_TIMEOUT_US - 1);
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_BUSY);
    cpc_fuji_card_advance(&card, 1);
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_ERROR);
    CHECK(read_port(&card, FUJI_PORT_ERROR) == FUJI_ERR_TIMEOUT);
    CHECK(!cpc_fuji_card_reply(&card, timed_out.ticket, timed_out.data, timed_out.size));

    cpc_fuji_request in_flight = ping(&card);
    CHECK(!cpc_fuji_card_reply(&card, timed_out.ticket, timed_out.data, timed_out.size));
    cpc_fuji_card_reset(&card);
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_IDLE);
    CHECK(read_port(&card, FUJI_PORT_ERROR) == FUJI_ERR_NONE);
    CHECK(!cpc_fuji_card_reply(&card, in_flight.ticket, in_flight.data, in_flight.size));

    cpc_fuji_request retry = ping(&card);
    CHECK(!cpc_fuji_card_reply(&card, in_flight.ticket, in_flight.data, in_flight.size));
    CHECK(cpc_fuji_card_reply(&card, retry.ticket, retry.data, retry.size));
    CHECK(read_port(&card, FUJI_PORT_STATUS) == FUJI_READY);
}

int main(void)
{
    check_decode();
    check_buffers();
    check_stale_replies();
    puts("FujiNet ROM selection, mailbox bounds, timeout, retry, and reset checks passed");
    return EXIT_SUCCESS;
}
