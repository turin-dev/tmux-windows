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
- [x] **Phase 7** — layout presets · pane resize · zoom · swap/rotate · mouse

## Install

Prebuilt Windows x64 downloads are attached to each
[Release](https://github.com/turin-dev/tmux-windows/releases):

- **Setup wizard** — `tmuxw-<version>-setup.exe`. A GUI installer with a license
  page, install-location picker, and an option to add tmuxw to your `PATH`. It
  installs both command names, `tmux` and `tmuxw`, and includes a clean
  uninstaller.
- **Portable** — `tmuxw-<version>-win-x64.zip` (or the bare `.exe`). No install;
  unzip and run. Statically linked, so no VC++ redistributable is required.
- **winget** — `winget install Turin.tmuxw` (both `tmux` and `tmuxw` commands).

## Requirements

- Windows 10 1809+ (build 17763) or Windows 11 — for the ConPTY API
- To build from source: Visual Studio 2022/2026 with the C++ toolset (MSVC,
  CMake, Ninja)

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

Once installed (or with `build\` on your `PATH`), the familiar `tmux` name works
too — `tmux` and `tmuxw` are the same binary:

```
tmux                 REM attach to (or start) the default session
tmux new             REM start a session and attach (alias: new-session)
tmux new -s work     REM start/attach a named session
tmux attach -t work  REM attach to an existing session (alias: a)
tmux ls              REM list running sessions (alias: list-sessions)
tmux kill-session -t work   REM stop a session's server
tmux kill-server     REM stop every tmuxw session
tmuxw --standalone   REM run in one process, no server (for debugging)
```

Each named session is an independent background server, so sessions detach and
reattach separately and `tmux ls` enumerates them.

### Key bindings (prefix = Ctrl-B)

| Key            | Action                          |
|----------------|---------------------------------|
| `Ctrl-B %`     | split active pane left \| right |
| `Ctrl-B "`     | split active pane top / bottom  |
| `Ctrl-B o`     | select next pane                |
| `Ctrl-B ←↑↓→`  | select pane by direction        |
| `Ctrl-B Ctrl-←↑↓→` | resize active pane (1 cell) |
| `Ctrl-B Alt-←↑↓→`  | resize active pane (5 cells)|
| `Ctrl-B q`     | show pane numbers (digit selects) |
| `Ctrl-B t`     | big-clock mode (any key exits)  |
| `Ctrl-B z`     | zoom / unzoom active pane       |
| `Ctrl-B Space` | cycle layout preset             |
| `Ctrl-B {` / `}` | swap pane with prev / next    |
| `Ctrl-B Ctrl-O`| rotate panes                    |
| `Ctrl-B ;`     | select last (previously active) pane |
| `Ctrl-B l`     | select last window              |
| `Ctrl-B !`     | break active pane into a new window |
| `Ctrl-B ]`     | paste the clipboard into the active pane |
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
| `w` / `b` / `e`         | next / previous / end of word          |
| `f` / `F` `<c>`         | jump to next / previous char `c`       |
| `t` / `T` `<c>`         | jump just before next / previous `c`   |
| `0` / `^` / `$`         | line start / first word / line end     |
| `PageUp` / `PageDown`   | scroll a full page                     |
| `Ctrl-U` / `Ctrl-D`     | scroll a half page                     |
| `H` / `M` / `L`         | top / middle / bottom of the viewport  |
| `g` / `G`               | jump to top / bottom of history        |
| `/` / `?`               | search forward / backward              |
| `n` / `N`               | repeat search, same / opposite way     |
| `Space` / `v`           | start / clear a characterwise selection |
| `V`                     | linewise selection                     |
| `Ctrl-V`                | rectangle (block) selection            |
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

Commands: `new-window`, `split-window [-h|-v]`, `select-pane [-U|-D|-L|-R|-t N]`,
`resize-pane [-U|-D|-L|-R [n]] [-Z]`, `select-layout <name>`, `next-layout`,
`rotate-window [-U]`, `swap-pane [-U|-D]`, `break-pane`, `join-pane -s N [-h|-v]`,
`paste-buffer`,
`last-pane`, `last-window`, `kill-pane`, `next-window`, `previous-window`,
`select-window -t N`, `swap-window [-s a] -t b`, `kill-window`,
`rename-window <name>`,
`rename-session <name>`, `copy-mode`, `detach-client`, `send-prefix`,
`send-keys <keys...>`, `display-panes`, `display-message <text>`,
`run-shell <cmd>`, `if-shell <cond> <then> [else]`, `clock-mode`,
`command-prompt`, `set <option> <value>`,
`bind <key> <command>`, `unbind <key>`, `source-file <path>`.

Multiple commands can be chained on one line (in a binding, config, or prompt)
with a standalone `;`, e.g. `bind S "split-window -v ; select-pane -D"`.

`bind -r <key> <command>` makes a binding *repeatable*: for a short window after
it runs you can press the key again without the prefix (handy for `bind -r H
resize-pane -L`).

Layout presets for `select-layout`: `even-horizontal`, `even-vertical`,
`main-horizontal`, `main-vertical`, `tiled` (also cycled with `Ctrl-B Space`).

Options for `set`: `prefix <key>`, `status on|off`, `mouse on|off`,
`base-index <n>` (first window number), `pane-base-index <n>`,
`status-left <fmt>`, `status-right <fmt>`. With `mouse on`, click a pane to
select it, drag a divider to resize, and use the scroll wheel to enter copy mode
and scroll back.

Status formats expand `#S` (session), `#W`/`#I` (current window name/index),
`#H` (host), `##` (literal `#`), and `%H %M %S %Y %m %d` (time). Defaults:
`status-left "[#S] "`, `status-right "%H:%M"`.

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
build\tmuxw.exe --selftest-mouse          REM mouse reporting + SGR parse + actions
ctest --test-dir build                    REM emu, layout, ipc, status, copymode, cmd
```

## License

tmuxw is released under the [MIT License](LICENSE). Bundled third-party
components and their licenses are listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
