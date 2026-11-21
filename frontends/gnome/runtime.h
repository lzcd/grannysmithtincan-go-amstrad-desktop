#ifndef CPC_DESKTOP_RUNTIME_H
#define CPC_DESKTOP_RUNTIME_H

#include <gio/gio.h>

GSubprocess *cpc_runtime_start(const char *dist, const char *data,
                               unsigned boip_port, unsigned web_port, GError **error);

#endif
