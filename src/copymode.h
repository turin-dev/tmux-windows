/* copymode.h — vi-style copy mode over a pane's scrollback.
 *
 * Entered with the prefix key + '['. While active, keys navigate the pane's
 * history (arrows / hjkl / PageUp/Down / g / G), Space starts a selection, and
 * Enter (or 'y') yanks the selection to the clipboard and exits. Esc or 'q'
 * cancels. Coordinates are absolute line indices into the pane screen's
 * scrollback + live grid (see screen_line_cell).
 */
#ifndef TMUXW_COPYMODE_H
#define TMUXW_COPYMODE_H

#include "model/pane.h"
#include "util/strbuf.h"

typedef struct copymode {
    int      active;
    pane_t  *pane;
    int      top;          /* absolute line shown at the top of the viewport */
    int      cur_line;
    int      cur_col;
    int      sel;          /* a selection is in progress */
    int      sel_mode;     /* 0 = characterwise, 1 = linewise, 2 = block */
    int      anchor_line;
    int      anchor_col;
    int      esc;          /* input parser: 0 normal, 1 after ESC, 2 in CSI */
    char     csi[8];
    int      csilen;
    /* incremental search (/ ? n N) */
    int      searching;    /* capturing a search query */
    int      search_dir;   /* +1 forward, -1 backward (last search direction) */
    char     query[128];
    int      qlen;
} copymode_t;

void copymode_enter(copymode_t *cm, pane_t *pane);
void copymode_exit(copymode_t *cm);

/* Feed input bytes. If a copy was requested, fills `text` (UTF-8) and returns 1
 * (mode also exits). Returns 0 otherwise. Sets active=0 on any exit. */
int  copymode_input(copymode_t *cm, const char *bytes, size_t n, strbuf_t *text);

/* Rendering helpers. */
int  copymode_top(const copymode_t *cm);
void copymode_cursor(const copymode_t *cm, int *vrow, int *vcol);
int  copymode_selected(const copymode_t *cm, int line, int col);

/* Search prompt state (for the status/indicator). Returns 1 while a query is
 * being typed, with the direction char ('/' or '?') and text via the outputs. */
int  copymode_search_prompt(const copymode_t *cm, char *dir, const char **query);

#endif /* TMUXW_COPYMODE_H */
