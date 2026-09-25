# Using Lemma

Lemma is currently a development project. Run it from a checkout rather than treating it as a
stable installed tool.

## Build and run

The supported development environment is Nix:

```sh
nix develop
just run
just run split --right

# Equivalent convenience command inside the development shell:
lemma
lemma split --right
```

`just run [args...]` is the canonical development entry point; the shell's `lemma` alias uses the
same runner. For bare `lemma`, `lemma new`, and `lemma start`, it supplies the invocation directory
when `--cwd` is omitted. See [Development](development.md#workflow) for build profiles and caching.

Each checkout or git worktree receives a stable private development runtime namespace. Rebuilding
the binary automatically replaces an older daemon in that namespace, so development commands do
not connect to an installed Lemma daemon or to another worktree. Release builds remain explicit for
packaging and production behavior:

```sh
nix build .#lemma
nix run .#lemma
```

The Nix shell supplies the pinned Ghostty source. A non-Nix build must initialize it first:

```sh
git submodule update --init --depth 1 third_party/ghostty
```

## Configuration

Use [Configuration](configuration.md) to customize keys, terminal history, status UI, launch
defaults, and Lua commands. Start with its [complete example](configuration.md#api) and validate
without changing a daemon:

```sh
lemma config check
lemma config check ./init.lua
```

## Sessions, tabs, and panes

Lemma's hierarchy is:

```text
Session -> Tab -> Pane
```

A per-user daemon owns sessions, child processes, PTYs, and terminal state. Creating the first
Session starts the daemon. The daemon exits when its final Session ends. Detaching a client leaves
pane processes running; killing the daemon does not.

```sh
lemma                              # create a numbered Session and attach
lemma new [NAME]                   # create a Session and attach
lemma start [NAME]                 # create a detached Session
lemma attach [NAME]
lemma list                         # alias: lemma ls
lemma rename OLD NEW
lemma kill NAME
```

An omitted `attach` target selects the most recently active detached Session. Creation accepts an
initial directory, an exit policy, and an exact command:

```sh
lemma new work --cwd "$PWD"
lemma start tests -- just test
lemma new report --hold -- ./produce-report
```

Arguments after `--` execute directly without shell interpretation. When `--cwd` or an exact
command is omitted, the configured launch default applies; without configuration Lemma uses the
account home and login shell. Pane processes normally keep running without `--hold`; `--hold`
retains the Pane and its terminal after the process exits.

`lemma --help` groups commands under **Basic**, **Resources**, **Automation**, and **Other**.
Use `lemma COMMAND --help` (or `lemma help COMMAND`) for behavior, options, and examples.

Basic pane commands are available directly:

```sh
lemma split --right                         # open another terminal beside this pane
lemma send --paste 'just test' --key enter   # type into the running program and press Enter
lemma wait --contains 'Ready' --timeout 10s  # wait for matching terminal content
lemma capture                              # print the current screen as text
lemma capture --source recent --lines 100   # include bounded scrollback
lemma focus --pane 1:1
lemma zoom --on                             # fill the tab with this pane; --off restores it
lemma resize right 5                        # move the divider five cells to the right
lemma swap --pane 0:1 1:1
```

`send` sends one ordered batch of text, paste, or key presses to an existing Pane; it does not launch
a process directly or press Enter automatically. `capture` prints terminal text, not an image, and
does not send input or move the viewport. `wait` blocks until the Pane's process exits or a specified
condition matches, with a default timeout of 30 seconds. Any process exit satisfies a conditionless
wait; use `--exit-code 0` to require success. `--until-prompt` requires shell-integration markers;
`--until-command` waits for the next shell-integration command completion, prints its exit code,
and exits 1 unless that code is 0.
Terminal conditions can match existing state; use `--after-generation` when newer state is required.
Waiting for an entire Session or Tab to end is not supported.

Inside a Lemma pane, omitted command targets resolve from the current Session and Pane IDs. Outside
Lemma, provide `--session NAME|ID` and `--pane ID` (or a positional Pane ID). Explicit selectors
can address another resource; Pane IDs are Session-scoped.

`split` prints the new Pane ID, `capture` prints captured text, and the other basic pane commands
succeed quietly. Failures go to stderr. Their exit status is 0 for success, 1 for execution failure,
or 2 for invalid arguments/requests. Add `--json` to any basic pane command for the canonical Proc
result, including IDs, generations, and failure details. Output behavior does not depend on whether
stdout is a terminal or a pipe.

The complete structured interface is also available through resource commands:

```sh
lemma tab new --session work --title tests -- just test
lemma pane split --session work --pane 0:1 --right
lemma pane capture --session work --pane 0:1
lemma pane --help
```

Resource commands and their `lemma proc DOMAIN COMMAND` forms print the same Proc envelope with one
nested Command result. The basic verbs use that same execution path; `send` maps to `pane.input`.
The interactive `:` prompt continues to use the resource grammar described below.

## Interactive controls

The default prefix is `C-b`.

| Binding | Effect |
| --- | --- |
| `C-b C-b` | Send a literal `C-b` |
| `C-b d` | Detach |
| `C-b :` | Open the interactive command line |
| `C-b ~` | Open the bounded message history viewer |
| `C-b %` / `C-b "` | Split left-right / top-bottom |
| `C-b h/j/k/l` | Focus a neighboring Pane |
| `C-b H/J/K/L` | Swap with a neighboring Pane |
| `C-b C-h/j/k/l` or `C-b M-h/j/k/l` | Move a divider by one cell |
| `C-b m` | Enter persistent resize mode; use arrows or `h/j/k/l`, then Escape or Enter |
| `C-b x` | Close the focused Pane |
| `C-b z` | Toggle Pane zoom |
| `C-b c` | Create a Tab |
| `C-b n/p` | Select the next / previous Tab |
| `C-b 0-9` | Select a numbered Tab |
| `C-b &` | Close the active Tab |
| `C-b P/N` | Move the active Tab left / right |
| `C-b R/r` | Rename the Session / active Tab |
| `C-b [` | Enter copy mode |
| `C-b /` / `C-b ?` | Search forward / backward in copy mode |

The mouse can focus panes, select tabs, create a tab from the status `+`, reorder tabs by dragging,
resize split dividers, select terminal text, and scroll canonical history. Mouse reports are sent to
the child when its active terminal modes request them. The focused pane's cursor keeps the shape
(block, underline, or bar), blink state, and color its application requests. Copy mode, native
prompts, and focused extension Surfaces use a steady block while they own the cursor. Detaching
resets the outer terminal to its own configured cursor.

The replaceable statusline extension supplies the normal top row. `C-b s` opens the shipped
[session manager](extensions.md#shipped-user-layer).

The status row is the only persistent interaction chrome. An active resize, copy, search, log, or command mode
replaces normal Session and Tab status with one flat, left-aligned mode row; mode labels and prompts
never cover pane content. Normal status returns immediately when the interaction ends.

## Command line

`C-b :` replaces the status row with an empty `:` prompt. Up recalls the most recently submitted
command, and Up/Down continue through the bounded per-attachment history. Tab completes command
words and live Session names, Enter runs the command, and Escape, `C-c`, or `C-g` cancels. Arrows,
Home/End, `C-a`/`C-e`, Backspace/Delete, `C-u`, and `C-w` edit the line. The row stays otherwise
empty while editing. After Enter, a failure closes the prompt and replaces the status row with a
left-aligned error message. The message clears after 1.5 seconds or on the next keyboard or mouse
input, immediately restoring Session and Tab status. Repeated failures restart the timeout.

Each Attachment retains the latest 16 status messages. `C-b ~` opens a timestamped, read-only
full-pane view with the newest messages at the bottom while the status row reads `LOG`. Use `k`/Up
and `j`/Down, PageUp/PageDown,
`g`/Home, and `G`/End to navigate; use `q`, Escape, Enter, `C-c`, or `C-g` to leave.

Command history is separately limited to 16 entries and is memory-only by default. See
[`history.file`](configuration.md#api) for persistence and failure behavior.

The grammar is the human, mutating subset of `lemma proc`: omit `proc` and omit selectors for the
current Session, Tab, and Pane. Quotes and backslashes group literal text without shell expansion.
For example:

```text
pane split --right
tab new --title tests
pane resize --left 5
switch work
```

`switch SESSION` moves the live client connection to an existing detached Session without
restarting the client. `attach SESSION` and `session switch SESSION` are aliases; none of these
creates a nested Session. Session names are completed from the daemon's live Session registry.
Native commands and registered Lua commands share command-line discovery and completion. Lua
commands run asynchronously in the isolated host and submit ordinary Procs. See
[Custom commands](configuration.md#custom-commands) for registration, invocation, bounds, and failure
behavior.

Command and copy-search editors are native; the shipped statusline extension presents their prompts. With
`ui.status_line = false`, their bindings are intentionally inert rather than capturing invisible
input.

## Copy mode

Copy mode uses Vim-shaped movement over Ghostty-owned history. Its status row includes the current
history position as `COPY [current/total]`. `h/j/k/l`, arrows, word movement, line movement, and page
movement update the copy cursor. `v`, `V`, and `C-v` start character, line, and block selection. `y`
or Enter copies the selection and leaves copy mode. Search replaces that row with the editable
`/query` or `?query` prompt; progress and feedback return to the same row without covering the pane.

`Super-c` and `Ctrl-Shift-c` copy the current copy-mode or mouse selection using bounded OSC 52
output. Application clipboard access is separate and denied by default; see the
[clipboard settings](configuration.md#api).

### Application clipboard

When application access is enabled, Lemma preserves the requested terminal protocol: OSC 52 for
plain text, or Kitty's OSC 5522 for MIME data. It does not translate a text request into a protocol
the outer terminal may not support or call an OS clipboard utility. In Ghostty 1.3.1, text works
through OSC 52; PNG clipboard access requires an OSC 5522-capable outer terminal such as Kitty.
The outer terminal's permissions still apply. OSC 52 writes have no acknowledgement; publication
does not prove that the outer terminal accepted the write.

Reads are bounded to 1 MiB and 30 seconds. OSC 52 has no request IDs: after an outstanding read is
cancelled, times out, or fails, further OSC 52 reads are denied on that attachment so a late reply
cannot reach a different request or Pane. Attach from a fresh outer terminal window to retry reads
without an old terminal reply still in flight. Writes and Kitty's correlated requests remain
available, subject to their usual permissions.

### Clipboard images

With an outer terminal supporting Kitty's OSC 5522 clipboard protocol:

```sh
lemma paste-image --session work --pane 0:1
```

The native command prompt also accepts `paste-image`. This explicit user action reads `image/png`,
validates it in a separate helper, saves a private PNG on the **daemon host**, and pastes only its
shell-quoted path plus a space. It neither sends image bytes as keyboard input nor presses Enter.
The target must remain the attached Session's focused Pane. Clipboard refusal, invalid PNG data,
ownership changes, or file errors fail the operation rather than falling back to text paste.

Files are saved under `$XDG_CACHE_HOME/lemma/clipboard`, or `$HOME/.cache/lemma/clipboard` when
`XDG_CACHE_HOME` is unset. The directory is owner-only and files have mode `0600`. Saved files
persist until you remove them; they are not extension resources or automatically deleted on detach.
The adjacent `lemma-clipboard-host` executable must accompany the installation. This is a terminal
protocol bridge, not an OS clipboard utility or an OSC 52 image-read fallback. The outer terminal's
own consent policy still applies. Transfers are limited to 1 MiB and time out after 30 seconds;
PNG validation/file creation has a separate 10-second deadline.

## Automation

Use one Proc for one or more ordered Commands and Events for an observation stream:

```sh
lemma send --json --session work --pane 0:1 --paste 'just test' --key enter
lemma wait --json --session work --pane 0:1 --until-prompt --timeout 2m
lemma proc --file proc.json
lemma events --session work --pane 0:1 --screen
lemma events --signals                     # bells, notifications, progress, command state
lemma api schema --json
```

See [Automation API](api.md) for the control model. `lemma skill` prints a version-matched,
Agent Skills-compatible `SKILL.md` intended for coding agents. Save it under a directory named
`lemma` in the skill location used by the agent host. For example, Pi discovers the shared location
below:

```sh
mkdir -p ~/.agents/skills/lemma
lemma skill > ~/.agents/skills/lemma/SKILL.md
```

Repeat the export after updating Lemma so the installed guide stays matched to the binary.

## Terminal compatibility

Panes use `TERM=lemma` and `COLORTERM=truecolor`. Lemma builds a dedicated
[terminfo entry](../terminfo/lemma.terminfo) with `tic -x` and installs it under `share/terminfo`.
It describes the virtual terminal inside a Pane, not the outer terminal. `TERMINFO` in each child
points to that entry, including when running directly from a build tree. Keep the resources with
the executable when relocating an installation; copying only the binary is insufficient.

The entry uses the xterm-256color base, adds direct-color and styled-underline declarations,
keeps its cursor shape and color capabilities, and omits the clipboard-write capability because
[application clipboard](#application-clipboard) writes are denied by default. A pane's cursor-style
reset selects a steady block, independent of the outer terminal's configured default. Ghostty's
terminal-name query reports the same identity as `TERM`.

An SSH destination also needs the entry to run terminfo-based applications under `TERM=lemma`.
Install it on that destination rather than setting `TERM` to the outer terminal's name. For example,
from a Lemma pane, `infocmp -x lemma | ssh HOST 'tic -x -'` installs it in the remote user's database.
This does not install a remote Lemma daemon.

### Window geometry and input

Lemma requests in-band size reports (mode 2048) from supporting outer terminals. These carry both
rows/columns and pixel dimensions, and take precedence over stale or pixel-less PTY proxy reports.
Other terminals use the PTY window size. Lemma restores the parent's reporting mode on exit.
Font-size changes update Pane pixel geometry even when the character grid stays unchanged.

Lemma keeps outer focus reporting (mode 1004) enabled while attached and restores it on exit. A Pane
that enables focus reporting receives `CSI I` when it becomes focused and `CSI O` when it stops
being focused. A Pane is focused while it is the focused Pane of the active Tab in an attached
Session and the outer terminal has focus; Pane focus changes, Tab and Session switches, detach,
reattach, and Pane exit therefore report focus, and a new attachment assumes the outer terminal is
focused. Each change is reported once, in order with the Pane's other input. A Pane that enables
focus reporting learns its state at the next change rather than immediately.

Mouse reports outside the current grid are discarded, never typed into the shell. Recognized
partial mouse, size, and clipboard reports have a fixed 30-second transport deadline, independent
of Escape-key timing; progress does not renew it. Bracketed paste remains opaque.

### Kitty graphics

Lemma retains Kitty images and placements in the daemon's native terminal state; it does not pass
application graphics escape sequences through to the outer terminal. PNG, RGB/RGBA, multipart
uploads, Unicode placeholders, relative placements, and animation are projected into Pane geometry.
The outer terminal must support Kitty graphics. Pixel cell dimensions come from its window-size
report, with an 8×16 fallback when unavailable.

Images are clipped to Pane bounds and extension UI coverage, reconstructed on reattach, and
repositioned on resize. Placeholder characters are not replayed to the outer terminal. Animation
uses native frame state and bounded presentation deadlines, not PTY-output polling. Slow uploads
pause animation advancement rather than repeatedly abandoning incomplete frames. Synchronized
Pane presentation retains finished images; a covering Surface suppresses those frozen images until
the Pane can be presented again.

Kitty 0.48.2 on Linux with software rendering can leave a new image invisible under a steady cursor
until text redraws, including after reattach. This was also reproduced without Lemma. Lemma preserves
the native cursor mode rather than changing it to hide the outer renderer's behavior.

Per-Pane image storage is bounded to 8 MiB. PNGs must be at most 4096×4096 and decode within that
bound. A composed Attachment supports at most 256 visible image fragments and 32 MiB of projected
image data. Native projection also bounds stored image/placement counts and lookup work. Exceeding
presentation capacity fails the attachment rather than drawing outside its bounds. Graphics output
is limited to 64 KiB per frame. Pixel-exact non-cell-aligned scaling uses incremental nearest-neighbor
sampling; ordinary cell-aligned images retain their original pixel transfer.

## Current limits

- A Session accepts one attached controller at a time.
- Session, process, terminal, and history state survive detach, not daemon death or reboot.
- Kitty file, temporary-file, and shared-memory image transports remain disabled; use direct uploads.
- The Glyph Protocol is disabled.
