/* test_ipc.c — headless tests for the IPC frame protocol.
 *
 * Frames are written to and read from an anonymous pipe (no named pipe / server
 * needed), verifying the length framing and the size pack/unpack helpers.
 */
#include "platform/ipc.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

static void test_frame_roundtrip(void)
{
    HANDLE r, w;
    unsigned char type;
    strbuf_t payload;
    strbuf_init(&payload);

    if (ipc_make_pair(&w, &r) != 0) {
        printf("FAIL: ipc_make_pair\n");
        failures++;
        return;
    }

    CHECK(ipc_write_frame(w, MSG_INPUT, "hello", 5) == 0, "write MSG_INPUT");
    CHECK(ipc_read_frame(r, &type, &payload) == 0, "read frame");
    CHECK(type == MSG_INPUT, "type is MSG_INPUT");
    CHECK(payload.len == 5 && memcmp(payload.data, "hello", 5) == 0, "payload == 'hello'");

    /* An empty-payload control frame. */
    CHECK(ipc_write_frame(w, MSG_DETACH, NULL, 0) == 0, "write empty MSG_DETACH");
    CHECK(ipc_read_frame(r, &type, &payload) == 0, "read empty frame");
    CHECK(type == MSG_DETACH && payload.len == 0, "empty control frame");

    strbuf_free(&payload);
    CloseHandle(r);
    CloseHandle(w);
}

static void test_multiple_frames(void)
{
    HANDLE r, w;
    unsigned char type;
    strbuf_t payload;
    int i;
    strbuf_init(&payload);

    if (ipc_make_pair(&w, &r) != 0) {
        printf("FAIL: ipc_make_pair (multi)\n");
        failures++;
        return;
    }
    for (i = 0; i < 3; i++)
        ipc_write_frame(w, MSG_OUTPUT, "AB", 2);

    for (i = 0; i < 3; i++) {
        int rc = ipc_read_frame(r, &type, &payload);
        if (rc != 0 || type != MSG_OUTPUT || payload.len != 2) {
            printf("FAIL: multi-frame %d\n", i);
            failures++;
        }
    }
    CHECK(1, "read 3 sequential frames");

    strbuf_free(&payload);
    CloseHandle(r);
    CloseHandle(w);
}

static void test_size_pack(void)
{
    unsigned char buf[4];
    int cols = 0, rows = 0;

    ipc_pack_size(buf, 200, 50);
    ipc_unpack_size(buf, 4, &cols, &rows);
    CHECK(cols == 200 && rows == 50, "size pack/unpack roundtrip");

    /* Values above 255 must survive (two-byte encoding). */
    ipc_pack_size(buf, 300, 100);
    ipc_unpack_size(buf, 4, &cols, &rows);
    CHECK(cols == 300 && rows == 100, "size > 255 survives");

    ipc_unpack_size(buf, 2, &cols, &rows);
    CHECK(cols == 80 && rows == 25, "short buffer -> default size");
}

int main(void)
{
    test_frame_roundtrip();
    test_multiple_frames();
    test_size_pack();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
