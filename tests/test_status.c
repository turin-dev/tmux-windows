/* test_status.c — headless tests for the status-bar renderer. */
#include "status.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

static void test_status_basic(void)
{
    strbuf_t out;
    status_win_t wins[2];
    strbuf_init(&out);

    wins[0].index = 0; wins[0].name = "pwsh"; wins[0].current = 0;
    wins[1].index = 1; wins[1].name = "cmd";  wins[1].current = 1;

    status_render(&out, 80, 25, "main", wins, 2, "13:37");
    strbuf_putc(&out, '\0');

    CHECK(strstr(out.data, "[main]") != NULL, "shows session name");
    CHECK(strstr(out.data, "0:pwsh") != NULL, "shows window 0");
    CHECK(strstr(out.data, "1:cmd*") != NULL, "marks current window with *");
    CHECK(strstr(out.data, "13:37") != NULL, "shows clock");
    CHECK(strstr(out.data, "\x1b[25;1H") != NULL, "positions at status row");
    CHECK(strstr(out.data, "\x1b[7m") != NULL, "uses reverse video");

    strbuf_free(&out);
}

static void test_status_width(void)
{
    /* The bar must be exactly `cols` cells between the position and the reset. */
    strbuf_t out;
    status_win_t w;
    const char *p, *reset;
    strbuf_init(&out);
    w.index = 0; w.name = "x"; w.current = 1;

    status_render(&out, 20, 3, "s", &w, 1, "00:00");
    strbuf_putc(&out, '\0');

    p = strstr(out.data, "\x1b[7m");
    reset = strstr(out.data, "\x1b[0m");
    CHECK(p != NULL && reset != NULL && reset > p, "bar delimited by SGR");
    CHECK((int)(reset - (p + 4)) == 20, "bar body is exactly cols wide");

    strbuf_free(&out);
}

int main(void)
{
    test_status_basic();
    test_status_width();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
