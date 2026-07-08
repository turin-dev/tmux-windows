/* render.h — composite a pane's screen grid into VT output.
 *
 * Phase 1 handles a single full-window pane: dirty rows are re-emitted as VT
 * (cursor positioning + SGR + UTF-8 text) into a byte buffer, which the caller
 * writes to the host terminal in one go. Later phases extend this to composite
 * multiple panes with borders at an (x, y) offset.
 */
#ifndef TMUXW_RENDER_H
#define TMUXW_RENDER_H

#include "emu/screen.h"
#include "model/pane.h"
#include "copymode.h"
#include "util/strbuf.h"

/* Append VT output for every dirty row of `s` to `out`, then position and
 * show/hide the host cursor to match the screen. Clears the screen's dirty
 * flags. Does not write to any handle — the caller flushes `out`. */
void render_frame(strbuf_t *out, screen_t *s);

/* Append a full-screen clear + home to `out` (used on (re)attach). */
void render_clear(strbuf_t *out);

/* Composite one pane's dirty rows into `out` at the pane's (x, y) offset,
 * clipped to its cols/rows. Does not touch the host cursor. Clears the pane
 * screen's dirty flags. */
void render_pane(strbuf_t *out, const pane_t *p);

/* Position the host cursor at the active pane's cursor (offset into the window)
 * and show or hide it to match. */
void render_active_cursor(strbuf_t *out, const pane_t *active);

/* Composite a pane in copy mode: its scrollback viewport with the selection
 * highlighted, a copy-mode indicator, and the copy cursor. */
void render_pane_copymode(strbuf_t *out, const pane_t *p, const copymode_t *cm);

#endif /* TMUXW_RENDER_H */
