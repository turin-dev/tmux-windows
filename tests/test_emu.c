/* test_emu.c — headless tests for the libvterm-backed screen + renderer.
 *
 * No console or ConPTY needed: feed known VT bytes into a screen, read the grid
 * back, and check the renderer emits the text. Run via ctest (target: emu).
 */
#include "emu/screen.h"
#include "model/pane.h"
#include "render.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

/* Read a grid row into a NUL-terminated ASCII string (best-effort: takes the
 * first code point of each cell, treating 0 as space). */
static void row_text(screen_t *s, int row, char *buf, int buflen)
{
    int cols = screen_cols(s), col, n = 0;
    for (col = 0; col < cols && n < buflen - 1; col++) {
        VTermScreenCell cell;
        if (!screen_get_cell(s, row, col, &cell) || cell.width == 0)
            continue;
        buf[n++] = (cell.chars[0] && cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
    }
    /* Trim trailing spaces. */
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
}

static void test_basic_text(void)
{
    screen_t *s = screen_new(20, 5);
    char buf[64];
    int r, c, vis;

    CHECK(s != NULL, "screen_new");
    CHECK(screen_cols(s) == 20 && screen_rows(s) == 5, "dimensions");

    screen_write(s, "Hello\r\nWorld", 12);

    row_text(s, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Hello") == 0, "row 0 == 'Hello'");
    row_text(s, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "World") == 0, "row 1 == 'World'");

    screen_cursor(s, &r, &c, &vis);
    CHECK(r == 1 && c == 5, "cursor after 'World' at (1,5)");

    screen_free(s);
}

static void test_sgr_color(void)
{
    /* Red 'R' then reset. Cell should carry a non-default fg colour. */
    screen_t *s = screen_new(10, 2);
    VTermScreenCell cell;
    screen_write(s, "\x1b[31mR\x1b[0m", 9);
    CHECK(screen_get_cell(s, 0, 0, &cell) == 1, "get red cell");
    CHECK(cell.chars[0] == 'R', "red cell char == 'R'");
    CHECK(!VTERM_COLOR_IS_DEFAULT_FG(&cell.fg), "red cell fg is non-default");
    screen_free(s);
}

static void test_render_output(void)
{
    screen_t *s = screen_new(20, 3);
    strbuf_t out;
    strbuf_init(&out);

    screen_write(s, "ABC", 3);
    render_frame(&out, s);
    strbuf_putc(&out, '\0');
    CHECK(strstr(out.data, "ABC") != NULL, "rendered frame contains 'ABC'");
    CHECK(strstr(out.data, "\x1b[?25") != NULL, "rendered frame toggles cursor");

    /* After a frame, dirty flags are cleared -> a second frame has no rows. */
    strbuf_clear(&out);
    render_frame(&out, s);
    strbuf_putc(&out, '\0');
    CHECK(strstr(out.data, "ABC") == NULL, "clean frame re-emits no text");

    strbuf_free(&out);
    screen_free(s);
}

static void test_resize(void)
{
    screen_t *s = screen_new(20, 5);
    screen_clear_dirty(s);
    CHECK(!screen_row_dirty(s, 0), "row 0 clean before resize");
    screen_resize(s, 30, 8);
    CHECK(screen_cols(s) == 30 && screen_rows(s) == 8, "resized dimensions");
    CHECK(screen_row_dirty(s, 0) && screen_row_dirty(s, 7), "resize marks all dirty");
    screen_free(s);
}

static void test_render_pane_offset(void)
{
    /* A pane at (x=5, y=2): screen row 0 must be drawn at host row 3, col 6. */
    pane_t p;
    strbuf_t out;
    memset(&p, 0, sizeof(p));
    p.screen = screen_new(10, 3);
    p.x = 5; p.y = 2; p.cols = 10; p.rows = 3;
    screen_write(p.screen, "XY", 2);

    strbuf_init(&out);
    render_pane(&out, &p);
    strbuf_putc(&out, '\0');
    CHECK(strstr(out.data, "XY") != NULL, "render_pane emits pane text");
    CHECK(strstr(out.data, "\x1b[3;6H") != NULL, "render_pane positions at pane origin");
    CHECK(strstr(out.data, "\x1b[2K") == NULL, "render_pane does not erase-to-EOL");

    strbuf_free(&out);
    screen_free(p.screen);
}

/* Read an absolute line (scrollback + live) into an ASCII string. */
static void abs_line_text(screen_t *s, int absrow, char *buf, int buflen)
{
    int cols = screen_cols(s), col, n = 0;
    for (col = 0; col < cols && n < buflen - 1; col++) {
        VTermScreenCell cell;
        if (!screen_line_cell(s, absrow, col, &cell) || cell.width == 0)
            continue;
        buf[n++] = (cell.chars[0] && cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
}

static void test_scrollback(void)
{
    /* Write more lines than fit; the oldest must land in scrollback. */
    screen_t *s = screen_new(10, 3);
    char buf[32];

    screen_write(s, "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5", 22);

    CHECK(screen_sb_count(s) >= 2, "scrollback captured scrolled lines");
    CHECK(screen_total_lines(s) == screen_sb_count(s) + 3, "total = sb + rows");

    abs_line_text(s, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "L0") == 0, "oldest scrollback line is 'L0'");

    /* The last live line should be reachable via the absolute coordinate too. */
    abs_line_text(s, screen_total_lines(s) - 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "L5") == 0, "newest line is 'L5'");

    screen_free(s);
}

int main(void)
{
    test_basic_text();
    test_sgr_color();
    test_render_output();
    test_resize();
    test_render_pane_offset();
    test_scrollback();

    if (failures == 0) {
        printf("\nALL PASSED\n");
        return 0;
    }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
