#ifndef CPCFUJI_H
#define CPCFUJI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CPC_FUJI_CAPACITY = 64,
    CPC_FUJI_PING = 1,
    CPC_FUJI_PING_DATA = 0x2a,
    CPC_FUJI_DIR_OPEN = 0x10,
    CPC_FUJI_DIR_NEXT = 0x11,
    CPC_FUJI_FILE_OPEN = 0x12,
    CPC_FUJI_FILE_NEXT = 0x13,
    CPC_FUJI_RESULT_OK = 0,
    CPC_FUJI_RESULT_END = 1,
    CPC_FUJI_RESULT_ERROR = 2,
    CPC_FUJI_DIR_OK = CPC_FUJI_RESULT_OK,
    CPC_FUJI_DIR_END = CPC_FUJI_RESULT_END,
    CPC_FUJI_DIR_ERROR = CPC_FUJI_RESULT_ERROR,
    CPC_FUJI_PROGRAM_LIMIT = 2048,
    CPC_FUJI_BOIP_PORT = 11510
};

typedef struct {
    uint64_t ticket;
    size_t size;
    uint8_t data[CPC_FUJI_CAPACITY];
} cpc_fuji_request;

#ifdef __cplusplus
}
#endif

#endif
