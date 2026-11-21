#ifndef FUJI_LINK_H
#define FUJI_LINK_H

#include "cpcfujilink.h"

// Adopt an already connected stream for protocol fixtures.
cpc_fuji_link *cpc_fuji_link_from_fd(int fd);

#endif
