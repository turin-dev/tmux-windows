/* pane.h — a single terminal pane: a child shell in a pseudo console, its
 * screen grid, and the plumbing to feed output in and route input out.
 *
 * A pane owns a reader thread that drains its pseudo console into a pending
 * buffer and signals a shared wake event; the main loop calls pane_pump() to
 * parse that buffer into the screen. Geometry (x, y, cols, rows) is the pane's
 * placement inside the window, assigned by the layout engine.
 */
#ifndef TMUXW_MODEL_PANE_H
#define TMUXW_MODEL_PANE_H

#include <windows.h>
#include <stddef.h>

#include "platform/conpty.h"
#include "emu/screen.h"
#include "util/strbuf.h"

typedef struct pane {
    int              id;
    conpty_t         pty;
    screen_t        *screen;
    int              x, y, cols, rows;  /* placement within the window */
    CRITICAL_SECTION lock;              /* guards `pending` */
    strbuf_t         pending;           /* raw pty output awaiting parse */
    HANDLE           reader;            /* output reader thread */
    HANDLE           wake;              /* shared main-loop event (not owned) */
    int              has_conpty;
} pane_t;

/* Spawn `cmdline` in a cols x rows pseudo console and start the reader thread.
 * `wake` is signalled whenever new output arrives. Returns NULL on failure. */
pane_t *pane_create(int id, const wchar_t *cmdline, int cols, int rows, HANDLE wake);

/* Stop the reader thread, close the pseudo console, free everything. */
void    pane_close(pane_t *p);

/* Resize the pseudo console and screen to the pane's current cols/rows. */
void    pane_apply_geometry(pane_t *p);

/* Parse any pending output into the screen (main thread). Returns bytes parsed. */
size_t  pane_pump(pane_t *p);

/* Forward input bytes to the pane's child shell. */
void    pane_write_input(pane_t *p, const char *bytes, size_t n);

/* True once the child process has exited. */
int     pane_child_exited(pane_t *p);

#endif /* TMUXW_MODEL_PANE_H */
