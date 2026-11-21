#ifndef CPC_DESKTOP_DEBUGGER_H
#define CPC_DESKTOP_DEBUGGER_H

#include "cpcdebug.h"
#include <gtk/gtk.h>

typedef struct {
    cpc_session *session;
    GtkWindow *window;
    GtkTextBuffer *screen;
    GtkEntry *memory, *breakpoint;
    GtkLabel *breakpoints_label;
    GArray *breakpoints;
} cpc_debugger;

void cpc_debugger_show(cpc_debugger *debugger, GtkWindow *parent);
void cpc_debugger_refresh(cpc_debugger *debugger);
void cpc_debugger_set_session(cpc_debugger *debugger, cpc_session *session);
void cpc_debugger_clear(cpc_debugger *debugger);

#endif
