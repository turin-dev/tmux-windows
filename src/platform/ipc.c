/* ipc.c — see ipc.h.
 *
 * The pipe is opened for OVERLAPPED (asynchronous) I/O. This matters because a
 * synchronous pipe handle serializes all operations on it: the server reads
 * client input on one thread while writing display output on another, and with
 * a synchronous handle the write would block behind the pending read. Overlapped
 * handles let the two directions proceed concurrently. Each read/write still
 * waits for its own completion, so callers see simple blocking semantics.
 */
#include "platform/ipc.h"

#include <stdio.h>
#include <stdlib.h>

#define IPC_MAX_FRAME (16u * 1024u * 1024u)   /* sanity cap on a frame payload */
#define IPC_BUFSZ     (64u * 1024u)

void ipc_pipe_name(wchar_t *out, size_t cap, const wchar_t *session)
{
    wchar_t user[256];
    DWORD n = (DWORD)(sizeof(user) / sizeof(user[0]));
    if (!GetUserNameW(user, &n))
        wcscpy_s(user, 256, L"user");
    if (session == NULL || session[0] == L'\0')
        session = L"default";
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\tmuxw-%s-%s", user, session);
}

/* Transfer exactly `n` bytes to/from an overlapped handle, waiting for each
 * operation to complete. Returns 0 on success, -1 on EOF/error. */
static int io_full(HANDLE h, void *buf, DWORD n, int writing)
{
    unsigned char *p = (unsigned char *)buf;
    DWORD total = 0;
    HANDLE ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ev == NULL)
        return -1;

    while (total < n) {
        OVERLAPPED ov;
        DWORD moved = 0;
        BOOL ok;

        memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);

        if (writing)
            ok = WriteFile(h, p + total, n - total, NULL, &ov);
        else
            ok = ReadFile(h, p + total, n - total, NULL, &ov);

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(ev);
            return -1;
        }
        if (!GetOverlappedResult(h, &ov, &moved, TRUE) || moved == 0) {
            CloseHandle(ev);
            return -1;
        }
        total += moved;
    }
    CloseHandle(ev);
    return 0;
}

int ipc_write_frame(HANDLE h, unsigned char type, const void *payload, uint32_t len)
{
    unsigned char header[5];
    header[0] = type;
    header[1] = (unsigned char)(len & 0xFF);
    header[2] = (unsigned char)((len >> 8) & 0xFF);
    header[3] = (unsigned char)((len >> 16) & 0xFF);
    header[4] = (unsigned char)((len >> 24) & 0xFF);
    if (io_full(h, header, 5, 1) != 0)
        return -1;
    if (len > 0 && io_full(h, (void *)payload, len, 1) != 0)
        return -1;
    return 0;
}

int ipc_read_frame(HANDLE h, unsigned char *type, strbuf_t *payload)
{
    unsigned char header[5];
    uint32_t len;

    strbuf_clear(payload);
    if (io_full(h, header, 5, 0) != 0)
        return -1;

    *type = header[0];
    len = (uint32_t)header[1] | ((uint32_t)header[2] << 8) |
          ((uint32_t)header[3] << 16) | ((uint32_t)header[4] << 24);
    if (len > IPC_MAX_FRAME)
        return -1;

    if (len > 0) {
        char *tmp = (char *)malloc(len);
        if (tmp == NULL)
            return -1;
        if (io_full(h, tmp, len, 0) != 0) {
            free(tmp);
            return -1;
        }
        strbuf_append(payload, tmp, len);
        free(tmp);
    }
    return 0;
}

void ipc_pack_size(unsigned char buf[4], int cols, int rows)
{
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;
    buf[0] = (unsigned char)(cols & 0xFF);
    buf[1] = (unsigned char)((cols >> 8) & 0xFF);
    buf[2] = (unsigned char)(rows & 0xFF);
    buf[3] = (unsigned char)((rows >> 8) & 0xFF);
}

void ipc_unpack_size(const unsigned char *buf, size_t len, int *cols, int *rows)
{
    if (len < 4) {
        *cols = 80;
        *rows = 25;
        return;
    }
    *cols = (int)buf[0] | ((int)buf[1] << 8);
    *rows = (int)buf[2] | ((int)buf[3] << 8);
}

/* Complete a possibly-pending ConnectNamedPipe on an overlapped handle. */
static int finish_connect(HANDLE h)
{
    OVERLAPPED ov;
    HANDLE ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    DWORD d;
    int rc = 0;
    if (ev == NULL)
        return -1;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = ev;

    if (ConnectNamedPipe(h, &ov)) {
        rc = 0; /* connected immediately */
    } else {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) {
            rc = 0;
        } else if (e == ERROR_IO_PENDING) {
            rc = (WaitForSingleObject(ev, INFINITE) == WAIT_OBJECT_0 &&
                  GetOverlappedResult(h, &ov, &d, TRUE)) ? 0 : -1;
        } else {
            rc = -1;
        }
    }
    CloseHandle(ev);
    return rc;
}

HANDLE ipc_server_listen(const wchar_t *pipename)
{
    HANDLE h = CreateNamedPipeW(
        pipename,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, IPC_BUFSZ, IPC_BUFSZ, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    if (finish_connect(h) != 0) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

HANDLE ipc_client_connect(const wchar_t *pipename, int timeout_ms)
{
    int waited = 0;
    for (;;) {
        HANDLE h = CreateFileW(pipename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;

        if (GetLastError() == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(pipename, 200))
                continue;
        }
        if (waited >= timeout_ms)
            return INVALID_HANDLE_VALUE;
        Sleep(50);
        waited += 50;
    }
}

int ipc_make_pair(HANDLE *server, HANDLE *client)
{
    wchar_t name[128];
    HANDLE s, c;

    _snwprintf_s(name, 128, _TRUNCATE, L"\\\\.\\pipe\\tmuxw-testpair-%lu",
                 (unsigned long)GetCurrentProcessId());

    s = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                         1, IPC_BUFSZ, IPC_BUFSZ, 0, NULL);
    if (s == INVALID_HANDLE_VALUE)
        return -1;

    c = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (c == INVALID_HANDLE_VALUE) {
        CloseHandle(s);
        return -1;
    }

    if (finish_connect(s) != 0) {
        CloseHandle(s);
        CloseHandle(c);
        return -1;
    }

    *server = s;
    *client = c;
    return 0;
}
