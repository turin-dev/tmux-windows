/* conpty.h — ConPTY (Windows Pseudo Console) wrapper.
 *
 * Spawns a child process (a shell) attached to a pseudo console, and exposes
 * the pipe endpoints the parent uses to feed input and read output. Requires
 * Windows 10 1809+ (the CreatePseudoConsole API family).
 */
#ifndef TMUXW_PLATFORM_CONPTY_H
#define TMUXW_PLATFORM_CONPTY_H

#include <windows.h>

typedef struct conpty {
    HPCON  hpc;           /* pseudo console handle */
    HANDLE input_write;   /* parent writes child stdin (keystrokes) here */
    HANDLE output_read;   /* parent reads child stdout/stderr from here */
    HANDLE process;       /* child process handle */
    HANDLE thread;        /* child main thread handle */
} conpty_t;

/* Spawn `cmdline` in a fresh pseudo console sized `cols` x `rows`, starting in
 * `cwd` (NULL/empty inherits the caller's current directory).
 * `cmdline` must be a mutable-safe command line (CreateProcessW may modify a
 * copy internally; we copy it ourselves). Returns 0 on success, else a Win32
 * error / HRESULT-style non-zero code. On failure `*pty` is left zeroed. */
int conpty_spawn(conpty_t *pty, const wchar_t *cmdline, short cols, short rows,
                  const wchar_t *cwd);

/* Resize the pseudo console. Returns 0 on success. */
int conpty_resize(conpty_t *pty, short cols, short rows);

/* Tear down: closes the pseudo console and all owned handles. Safe on a
 * zeroed / partially-initialized struct. Does not wait for the child. */
void conpty_close(conpty_t *pty);

#endif /* TMUXW_PLATFORM_CONPTY_H */
