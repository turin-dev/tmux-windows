/* winterm.h — real (host) console setup for a tmuxw client.
 *
 * Puts the attached terminal into a raw, VT-in / VT-out mode so we can stream
 * bytes straight to/from a pseudo console, and restores the previous state on
 * teardown.
 */
#ifndef TMUXW_PLATFORM_WINTERM_H
#define TMUXW_PLATFORM_WINTERM_H

#include <windows.h>

typedef struct winterm {
    HANDLE in;         /* STD_INPUT_HANDLE  */
    HANDLE out;        /* STD_OUTPUT_HANDLE */
    DWORD  old_in_mode;
    DWORD  old_out_mode;
    UINT   old_in_cp;
    UINT   old_out_cp;
    BOOL   active;
} winterm_t;

/* Enter raw VT mode: VT output processing on, VT input on, line/echo/processed
 * input off, code pages set to UTF-8. Returns 0 on success. */
int  winterm_enable(winterm_t *t);

/* Current visible window size in character cells. */
void winterm_size(winterm_t *t, short *cols, short *rows);

/* Restore the console modes and code pages captured by winterm_enable. */
void winterm_restore(winterm_t *t);

#endif /* TMUXW_PLATFORM_WINTERM_H */
