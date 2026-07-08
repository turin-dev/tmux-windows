/* status.c — see status.h. */
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy `s` into row[pos..cols), returning the number of chars written. */
static int put_str(char *row, int cols, int pos, const char *s)
{
    int n = 0;
    while (*s && pos + n < cols) {
        row[pos + n] = *s++;
        n++;
    }
    return n;
}

void status_render(strbuf_t *out, int cols, int status_row,
                   const char *session_name,
                   const status_win_t *wins, int nwins,
                   const char *clock)
{
    char *row;
    int pos = 0, i;

    if (cols < 1)
        return;

    row = (char *)malloc((size_t)cols + 1);
    if (row == NULL)
        return;
    memset(row, ' ', (size_t)cols);
    row[cols] = '\0';

    pos += put_str(row, cols, pos, "[");
    pos += put_str(row, cols, pos, session_name ? session_name : "0");
    pos += put_str(row, cols, pos, "] ");

    for (i = 0; i < nwins; i++) {
        char seg[96];
        _snprintf_s(seg, sizeof(seg), _TRUNCATE, "%d:%s%s ",
                    wins[i].index,
                    wins[i].name ? wins[i].name : "",
                    wins[i].current ? "*" : "");
        pos += put_str(row, cols, pos, seg);
    }

    if (clock && *clock) {
        int cl = (int)strlen(clock);
        int start = cols - cl;
        if (start >= pos && start >= 0)
            memcpy(row + start, clock, (size_t)cl);
    }

    strbuf_printf(out, "\x1b[%d;1H", status_row);
    strbuf_append(out, "\x1b[7m", 4);       /* reverse-video bar */
    strbuf_append(out, row, (size_t)cols);
    strbuf_append(out, "\x1b[0m", 4);

    free(row);
}
