/* screen.h — a single pane's terminal screen, backed by libvterm.
 *
 * Feed the raw byte stream coming out of a pseudo console into screen_write();
 * libvterm parses the VT sequences and maintains a cell grid. The renderer then
 * reads cells back out to composite them onto the host terminal. Per-row dirty
 * flags let the renderer redraw only what changed.
 *
 * A screen is single-threaded: all calls must come from the same thread.
 */
#ifndef TMUXW_EMU_SCREEN_H
#define TMUXW_EMU_SCREEN_H

#include <stddef.h>
#include "vterm.h"

typedef struct screen screen_t;

/* Create a `cols` x `rows` screen. Returns NULL on allocation failure. */
screen_t *screen_new(int cols, int rows);
void      screen_free(screen_t *s);

/* Parse `len` bytes of pseudo-console output into the grid, updating dirty
 * rows, cursor position and visibility. */
void screen_write(screen_t *s, const char *bytes, size_t len);

/* Resize the grid. Marks every row dirty. */
void screen_resize(screen_t *s, int cols, int rows);

int  screen_cols(const screen_t *s);
int  screen_rows(const screen_t *s);

/* Cursor state as of the last screen_write. */
void screen_cursor(const screen_t *s, int *row, int *col, int *visible);

/* Read one cell. Returns 1 if the cell is valid (in bounds), else 0. */
int  screen_get_cell(const screen_t *s, int row, int col, VTermScreenCell *cell);

/* Convert a cell colour to concrete RGB using this screen's palette. */
void screen_color_rgb(const screen_t *s, VTermColor *col);

/* Dirty tracking for incremental rendering. */
int  screen_row_dirty(const screen_t *s, int row);
int  screen_has_dirty(const screen_t *s);   /* any dirty row or cursor move */
void screen_mark_all_dirty(screen_t *s);
void screen_clear_dirty(screen_t *s);

/* Scrollback. Absolute line coordinates span [0, screen_total_lines): the first
 * screen_sb_count() lines are history (oldest first), the rest are the live
 * grid. Used by copy mode to scroll back through output. */
int  screen_sb_count(const screen_t *s);
int  screen_total_lines(const screen_t *s);
int  screen_line_cell(const screen_t *s, int absrow, int col, VTermScreenCell *cell);

#endif /* TMUXW_EMU_SCREEN_H */
