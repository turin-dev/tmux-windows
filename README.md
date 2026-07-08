# tmuxw

A native Windows terminal multiplexer — a tmux clone built on the Windows
**ConPTY** (Pseudo Console) API, with **no WSL/Cygwin/MSYS2 dependency**.

## Status

Early development. Milestones (see the full plan for detail):

- [x] **Phase 0** — scaffolding + ConPTY passthrough (spawn a shell, stream I/O)
- [x] **Phase 1** — terminal emulation via libvterm (per-pane screen grid + renderer)
- [x] **Phase 2** — multiple panes + binary layout tree + prefix key + compositor
- [x] **Phase 3** — client/server split over Named Pipes (detach / attach)
- [x] **Phase 4** — windows (tabs) + status bar (window list + clock)
- [x] **Phase 5** — scrollback + vi-style copy mode + clipboard
- [x] **Phase 6** — command system + key bindings + config file
- [ ] Phase 7 — layout presets, pane resize, mouse

## Requirements

- Windows 10 1809+ (build 17763) or Windows 11 — for the ConPTY API
- Visual Studio 2022/2026 with the C++ toolset (MSVC, CMake, Ninja)

## Build

This repo vendors [libvterm](https://github.com/neovim/libvterm) as a git
submodule, so clone recursively:

```
git clone --recursive <repo-url>
REM or, in an existing clone:
git submodule update --init --recursive
```

Then build with the bundled VS toolchain:

```
scripts\build.bat            REM Debug build -> build\tmuxw.exe
scripts\build.bat Release
```

Or manually from a Developer prompt:

```
cmake -S . -B build -G Ninja
cmake --build build
```

## Run

```
build\tmuxw.exe              REM auto-detected shell (pwsh > powershell > cmd)
build\tmuxw.exe cmd.exe      REM a specific command line
```

Running `tmuxw` starts a background **server** (if one isn't already running)
that owns the session's panes, and attaches this terminal as a thin **client**.
The server survives detach: `Ctrl-B d` leaves the client and keeps the shells
running; run `tmuxw` again to reattach. Each pane runs a shell inside a pseudo
console; output is parsed by libvterm into a cell grid, composited by the server,
and streamed to the client. Exit the last pane to end the session.

```
tmuxw                REM attach to (or start) the default session
tmuxw --standalone   REM run in one process, no server (for debugging)
```

### Key bindings (prefix = Ctrl-B)

| Key            | Action                          |
|----------------|---------------------------------|
| `Ctrl-B %`     | split active pane left \| right |
| `Ctrl-B "`     | split active pane top / bottom  |
| `Ctrl-B o`     | select next pane                |
| `Ctrl-B ←↑↓→`  | select pane by direction        |
| `Ctrl-B x`     | kill active pane                |
| `Ctrl-B c`     | create a new window (tab)       |
| `Ctrl-B n` / `p`| next / previous window         |
| `Ctrl-B 0`–`9` | select window by number         |
| `Ctrl-B &`     | kill the current window         |
| `Ctrl-B [`     | enter copy mode (scrollback)    |
| `Ctrl-B :`     | command prompt                  |
| `Ctrl-B d`     | detach (server keeps running)   |
| `Ctrl-B Ctrl-B`| send a literal Ctrl-B           |

Bindings are configurable — see the config file below.

A status bar along the bottom row shows the session name, the window list (the
current window marked `*`), and a clock.

### Copy mode (`Ctrl-B [`)

Scroll back through a pane's history and copy text to the Windows clipboard:

| Key                     | Action                                 |
|-------------------------|----------------------------------------|
| `↑↓←→` / `h j k l`      | move the cursor                        |
| `PageUp` / `PageDown`   | scroll a page (also `Ctrl-U`/`Ctrl-D`) |
| `g` / `G`               | jump to top / bottom of history        |
| `Space`                 | start / clear a selection              |
| `Enter` or `y`          | copy the selection and exit            |
| `q` / `Esc`             | cancel                                 |

### Configuration

On startup the server reads `%USERPROFILE%\.tmuxw.conf` (if present) — a list of
commands, one per line (`#` starts a comment). The same commands work at the
command prompt (`Ctrl-B :`). See [`tmuxw.conf.example`](tmuxw.conf.example).

```
set prefix C-a              # change the prefix key
set status on              # show/hide the status bar
bind | split-window -h     # bind a key to a command
unbind '"'
```

Commands: `new-window`, `split-window [-h|-v]`, `select-pane [-U|-D|-L|-R]`,
`kill-pane`, `next-window`, `previous-window`, `select-window -t N`,
`kill-window`, `rename-window <name>`, `copy-mode`, `detach-client`,
`send-prefix`, `command-prompt`, `set <option> <value>`, `bind <key> <command>`,
`unbind <key>`, `source-file <path>`.

### Headless self-tests

Useful without an interactive terminal (also run in CI):

```
build\tmuxw.exe --selftest [cmd]          REM ConPTY output straight to stdout
build\tmuxw.exe --selftest-render [cmd]   REM ConPTY -> libvterm -> print grid
build\tmuxw.exe --selftest-split          REM two panes + layout + compositor
build\tmuxw.exe --selftest-ipc            REM full server/client round trip
build\tmuxw.exe --selftest-windows        REM window (tab) switching + status bar
build\tmuxw.exe --selftest-copymode       REM enter/exit copy mode via the session
build\tmuxw.exe --selftest-cmd            REM command system + bindings + config
ctest --test-dir build                    REM emu, layout, ipc, status, copymode, cmd
```

## License

tmuxw is released under the [MIT License](LICENSE). Bundled third-party
components and their licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
