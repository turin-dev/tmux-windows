/* status.h — render the bottom status bar (session name, window list, clock).
 *
 * Pure: it only appends VT bytes to a buffer, so it can be unit-tested without
 * a terminal. The session gathers the window list and clock and calls it.
 */
#ifndef TMUXW_STATUS_H
#define TMUXW_STATUS_H

#include "util/strbuf.h"

typedef struct status_win {
    int         index;
    const char *name;
    int         current;
} status_win_t;

/* Draw a `cols`-wide status bar at `status_row` (1-based): `left` text, then the
 * window list (current window marked with '*'), then `right` text right-aligned.
 * `left`/`right` are already-expanded strings. Rendered in reverse video. */
void status_render(strbuf_t *out, int cols, int status_row,
                   const char *left,
                   const status_win_t *wins, int nwins,
                   const char *right);

#endif /* TMUXW_STATUS_H */
