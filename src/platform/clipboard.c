/* clipboard.c — see clipboard.h. */
#include "platform/clipboard.h"

#include <windows.h>
#include <stdlib.h>

int clipboard_set_utf8(const char *utf8, size_t len)
{
    int wlen;
    HGLOBAL mem;
    wchar_t *dst;

    if (utf8 == NULL || len == 0)
        return 1;

    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (wlen <= 0)
        return 1;

    mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wlen + 1) * sizeof(wchar_t));
    if (mem == NULL)
        return 1;

    dst = (wchar_t *)GlobalLock(mem);
    if (dst == NULL) {
        GlobalFree(mem);
        return 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, dst, wlen);
    dst[wlen] = L'\0';
    GlobalUnlock(mem);

    if (!OpenClipboard(NULL)) {
        GlobalFree(mem);
        return 1;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, mem) == NULL) {
        /* Ownership stays with us on failure. */
        CloseClipboard();
        GlobalFree(mem);
        return 1;
    }
    /* On success the system owns `mem`; do not free it. */
    CloseClipboard();
    return 0;
}

char *clipboard_get_utf8(size_t *len)
{
    HANDLE h;
    wchar_t *w;
    int n;
    char *out = NULL;

    if (len) *len = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return NULL;
    if (!OpenClipboard(NULL))
        return NULL;

    h = GetClipboardData(CF_UNICODETEXT);
    if (h == NULL) { CloseClipboard(); return NULL; }
    w = (wchar_t *)GlobalLock(h);
    if (w == NULL) { CloseClipboard(); return NULL; }

    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 0) {
        out = (char *)malloc((size_t)n);
        if (out) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
            if (len) *len = (size_t)(n - 1);   /* exclude the terminating NUL */
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
    return out;
}
