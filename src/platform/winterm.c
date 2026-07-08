/* winterm.c — see winterm.h. */
#include "platform/winterm.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

int winterm_enable(winterm_t *t)
{
    DWORD out_mode, in_mode;

    t->active = FALSE;
    t->in = GetStdHandle(STD_INPUT_HANDLE);
    t->out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (t->in == INVALID_HANDLE_VALUE || t->out == INVALID_HANDLE_VALUE)
        return (int)ERROR_INVALID_HANDLE;

    if (!GetConsoleMode(t->out, &t->old_out_mode))
        return (int)GetLastError();
    if (!GetConsoleMode(t->in, &t->old_in_mode))
        return (int)GetLastError();

    /* Output: emit VT sequences verbatim, and do not force CR on wrap so the
     * child's own line discipline is preserved. */
    out_mode = t->old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
               DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(t->out, out_mode))
        return (int)GetLastError();

    /* Input: deliver keystrokes as a raw VT byte stream. Clearing LINE/ECHO/
     * PROCESSED means Ctrl+C etc. flow through to the child instead of raising
     * a console control event. */
    in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(t->in, in_mode)) {
        SetConsoleMode(t->out, t->old_out_mode);
        return (int)GetLastError();
    }

    t->old_out_cp = GetConsoleOutputCP();
    t->old_in_cp = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    t->active = TRUE;
    return 0;
}

void winterm_size(winterm_t *t, short *cols, short *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(t->out, &csbi)) {
        *cols = (short)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        *rows = (short)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
    } else {
        *cols = 80;
        *rows = 25;
    }
}

void winterm_restore(winterm_t *t)
{
    if (!t->active)
        return;
    SetConsoleMode(t->out, t->old_out_mode);
    SetConsoleMode(t->in, t->old_in_mode);
    SetConsoleOutputCP(t->old_out_cp);
    SetConsoleCP(t->old_in_cp);
    t->active = FALSE;
}
