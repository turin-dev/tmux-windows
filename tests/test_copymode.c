/* test_copymode.c — headless tests for copy mode over a pane's scrollback. */
#include "copymode.h"
#include "model/pane.h"
#include "emu/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

/* A pane with a real screen but no ConPTY. */
static pane_t *make_pane(int cols, int rows)
{
    pane_t *p = (pane_t *)calloc(1, sizeof(*p));
    InitializeCriticalSection(&p->lock);
    strbuf_init(&p->pending);
    p->screen = screen_new(cols, rows);
    p->cols = cols;
    p->rows = rows;
    p->has_conpty = 0;
    return p;
}

static void free_pane(pane_t *p)
{
    if (!p) return;
    if (p->screen) screen_free(p->screen);
    strbuf_free(&p->pending);
    DeleteCriticalSection(&p->lock);
    free(p);
}

static void feed(copymode_t *cm, const char *keys, strbuf_t *text)
{
    copymode_input(cm, keys, strlen(keys), text);
}

static void test_enter_and_scroll(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;
    int vr, vc;

    /* 5 lines through a 3-row screen -> 2 lines of scrollback. */
    screen_write(p->screen, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE", 28);
    CHECK(screen_sb_count(p->screen) >= 2, "scrollback present");

    copymode_enter(&cm, p);
    CHECK(cm.active, "copy mode active after enter");

    /* Jump to the very top; the viewport should show the oldest line. */
    feed(&cm, "g", NULL);
    copymode_cursor(&cm, &vr, &vc);
    CHECK(copymode_top(&cm) == 0, "'g' scrolls to top of history");
    CHECK(vr == 0, "cursor at viewport top after 'g'");

    free_pane(p);
}

static void test_select_and_copy(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;
    strbuf_t text;
    strbuf_init(&text);

    screen_write(p->screen, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE", 28);
    copymode_enter(&cm, p);

    /* Top, start selection, extend down one line, yank. */
    feed(&cm, "g", &text);
    CHECK(copymode_selected(&cm, 0, 0) == 0, "no selection before Space");
    feed(&cm, " ", &text);           /* start selection at top-left */
    CHECK(copymode_selected(&cm, 0, 0) == 1, "cell selected after Space");
    feed(&cm, "j", &text);           /* extend down a line */

    text.len = 0;
    feed(&cm, "\r", &text);          /* Enter yanks + exits */
    strbuf_putc(&text, '\0');

    CHECK(!cm.active, "copy mode exits after yank");
    CHECK(strstr(text.data, "AAAA") != NULL, "yanked text includes first line");

    strbuf_free(&text);
    free_pane(p);
}

static void test_escape_cancels(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;
    screen_write(p->screen, "AAAA\r\nBBBB\r\nCCCC", 16);
    copymode_enter(&cm, p);
    feed(&cm, "\x1b", NULL);          /* lone ESC cancels */
    CHECK(!cm.active, "ESC cancels copy mode");

    copymode_enter(&cm, p);
    feed(&cm, "q", NULL);
    CHECK(!cm.active, "'q' cancels copy mode");
    free_pane(p);
}

static void test_arrow_keys(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;
    int vr0, vc0, vr1, vc1;
    screen_write(p->screen, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE", 28);
    copymode_enter(&cm, p);

    copymode_cursor(&cm, &vr0, &vc0);
    feed(&cm, "\x1b[A", NULL);        /* Up arrow */
    copymode_cursor(&cm, &vr1, &vc1);
    CHECK(cm.active && (vr1 < vr0 || copymode_top(&cm) >= 0), "up arrow moves cursor up");

    free_pane(p);
}

static char cell_ch(pane_t *p, int line, int col)
{
    VTermScreenCell c;
    if (screen_line_cell(p->screen, line, col, &c) && c.chars[0] && c.chars[0] < 128)
        return (char)c.chars[0];
    return ' ';
}

static void test_search(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;

    /* foo at lines 0 and 2. */
    screen_write(p->screen, "foo\r\nbar\r\nfoo\r\nbaz\r\nqux", 22);
    copymode_enter(&cm, p);

    feed(&cm, "g", NULL);              /* top, line 0 */
    feed(&cm, "/foo\r", NULL);         /* forward search past the cursor */
    CHECK(cm.cur_line == 2 && cell_ch(p, cm.cur_line, cm.cur_col) == 'f',
          "forward search jumps to the next foo (line 2)");
    CHECK(!cm.searching, "search input state cleared after Enter");

    feed(&cm, "n", NULL);             /* repeat -> wraps to the other foo */
    CHECK(cm.cur_line == 0, "n wraps forward to line 0");

    feed(&cm, "N", NULL);             /* reverse -> back to line 2 */
    CHECK(cm.cur_line == 2, "N searches backward to line 2");

    /* A query with no match leaves the cursor put. */
    feed(&cm, "/zzz\r", NULL);
    CHECK(cm.cur_line == 2, "no-match search does not move the cursor");

    free_pane(p);
}

static void test_motions(void)
{
    pane_t *p = make_pane(20, 2);
    copymode_t cm;
    /* line 0: "the quick fox"  (t0 h1 e2 _3 q4 u5 i6 c7 k8 _9 f10 o11 x12) */
    screen_write(p->screen, "the quick fox\r\nsecond line here", 31);
    copymode_enter(&cm, p);

    feed(&cm, "g", NULL);              /* top line, col 0 */
    feed(&cm, "$", NULL);
    CHECK(cm.cur_col == 12, "$ moves to last non-space");
    feed(&cm, "0", NULL);
    CHECK(cm.cur_col == 0, "0 moves to start of line");
    feed(&cm, "w", NULL);
    CHECK(cm.cur_col == 4, "w moves to next word (quick)");
    feed(&cm, "w", NULL);
    CHECK(cm.cur_col == 10, "w moves to next word (fox)");
    feed(&cm, "b", NULL);
    CHECK(cm.cur_col == 4, "b moves back to previous word");
    feed(&cm, "e", NULL);
    CHECK(cm.cur_col == 8, "e moves to end of word");

    /* 'v' toggles selection like Space. */
    feed(&cm, "v", NULL);
    CHECK(copymode_selected(&cm, cm.cur_line, cm.cur_col) == 1, "v starts a selection");

    free_pane(p);
}

static void test_visual_modes(void)
{
    pane_t *p = make_pane(10, 3);
    copymode_t cm;

    screen_write(p->screen, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE", 28);
    copymode_enter(&cm, p);
    feed(&cm, "g", NULL);              /* top line 0 */

    /* Linewise: V then down selects whole lines 0 and 1. */
    feed(&cm, "V", NULL);
    feed(&cm, "j", NULL);
    CHECK(copymode_selected(&cm, 0, 9) == 1, "linewise selects far column of line 0");
    CHECK(copymode_selected(&cm, 1, 0) == 1, "linewise selects line 1");
    CHECK(copymode_selected(&cm, 2, 0) == 0, "linewise stops at line 1");
    feed(&cm, "V", NULL);             /* toggle off */
    CHECK(copymode_selected(&cm, 0, 0) == 0, "V again clears the selection");

    /* Block: Ctrl-V, move down+right, only the rectangle is selected. */
    feed(&cm, "g", NULL);
    feed(&cm, "\x16", NULL);          /* Ctrl-V at (0,0) */
    feed(&cm, "j", NULL);             /* -> line 1 */
    feed(&cm, "l", NULL);             /* -> col 1 */
    CHECK(copymode_selected(&cm, 0, 0) == 1, "block includes anchor corner");
    CHECK(copymode_selected(&cm, 1, 1) == 1, "block includes opposite corner");
    CHECK(copymode_selected(&cm, 0, 5) == 0, "block excludes columns outside the rect");

    free_pane(p);
}

int main(void)
{
    test_enter_and_scroll();
    test_select_and_copy();
    test_escape_cancels();
    test_arrow_keys();
    test_search();
    test_motions();
    test_visual_modes();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
