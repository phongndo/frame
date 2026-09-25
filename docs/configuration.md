# Configuration and commands

Lemma loads user configuration from Lua in a separate bounded host process. The daemon never
executes Lua while routing input, processing PTY bytes, or composing frames. A successful load is
validated and compiled into one immutable native configuration generation before any Session can
use it.

The host covers compiled input policy, terminal history, shipped status UI, launch defaults, and
asynchronous Lua commands in the interactive command line. It does not expose Event subscriptions,
custom UI Surfaces, or dynamic routing contexts. Language-neutral runtime extensions
use a separate [framed protocol](extensions.md).

## Location and validation

The default file is:

```text
$XDG_CONFIG_HOME/lemma/init.lua
```

If `XDG_CONFIG_HOME` is unset or not absolute, Lemma uses:

```text
$HOME/.config/lemma/init.lua
```

`LEMMA_CONFIG` selects another file. The host loads the shipped declarations even when the user file
is absent.
Validate a file without starting or changing the daemon:

```sh
lemma config check
lemma config check ./init.lua
```

The host has a 64 MiB Lua allocation bound. Startup must produce a complete configuration within two
seconds, the combined encoded configuration and command declarations are bounded to 64 KiB, and at
most 240 effective bindings are accepted. A syntax error, runtime error, unknown option, invalid binding, capacity failure, timeout,
or host crash rejects the complete generation. Daemon startup then continues with built-in
configuration.

Configuration and modules execute with the user's operating-system permissions. Only load code you
trust. `require()` searches beside `init.lua` before the ordinary Lua module path.

## Reload

Run `lemma config reload` or `reload` in the native `:` prompt. There is no configuration-file
watcher. The command uses `config.reload`, an ordinary Proc Command, and waits for publication or
rejection. An independent `lemma-config-host` evaluates Lua while panes continue processing input
and output. The native loader admits one bounded read quantum per reactor turn and validates the
complete bounded registration before replacing the generation.

Reload replaces keybindings, context policy, custom command declarations and their host,
outer-terminal presentation, and launch/scrollback defaults for future panes. It does not restart pane processes, change existing
scrollback limits, or alter stable IDs. Active native editors, copy/search, resize/prefix contexts,
and deferred prefix triggers are cancelled on publication; press-time key-release ownership is
preserved. Old hosted invocations and their remaining Procs are cancelled, and their process group
is revoked. Already completed effects are not rolled back. Managed status UI and independently
connected extensions are not restarted.

Changes to `history.file`, `ui.status_line`, or managed extension declarations currently require a
daemon restart. Such a change rejects the **entire** reload with `restart_required`; it never applies
only part of the new configuration. Lua errors, malformed output, and the two-second startup deadline
also retain the old generation. Unlike invalid startup configuration, a failed reload never falls
back to defaults. Unreadable files, existing non-regular paths, dangling symlinks, and other
filesystem errors reject reload. Removing the optional discovered file intentionally loads built-ins;
a missing explicitly selected `LEMMA_CONFIG` file is an error. Use `lemma config check` first to
validate syntax and declarations; it does not check whether a running daemon can apply a setting live.

Only one candidate loads at a time. Another request reports a conflict while loading or retiring
its previous host. A CONTROL/extension owner disconnected before publication cancels its candidate.
The native prompt requests daemon-wide reload and can finish even if its originating client detaches.
Explicit reload also recovers a failed command host; a bad candidate cannot remove working commands.

## API

This [complete configuration](../examples/configuration.lua) starts from the default bindings and
adds Meta-d to split a Pane and Meta-r to enter resize mode. Save it as `init.lua`:

```lua example=../examples/configuration.lua
local lemma = require("lemma")
local keymap = lemma.keymap
local ctx = lemma.context

lemma.setup({
  input = { preset = "default", prefix = "C-b" },
  terminal = { scrollback_lines = 100000 },
  ui = { status_line = true },
  launch = { default_program = { "/bin/sh", "-l" } },
})

ctx.set("resize", {
  label = "RESIZE",
  lifetime = "persistent",
  unbound = "consume",
})

keymap.set("normal", "M-d", "split_left_right")
keymap.set("normal", "M-r", ctx.push("resize"))
keymap.set("resize", "q", ctx.pop())
keymap.del("normal", "Cmd-c")
```

All `lemma.setup()` groups and fields are optional:

- `input.preset` is `"default"` or `"none"`. The default preset seeds ordinary context and binding
  declarations; `none` retains the routing-context slots but starts with no bindings. Both are
  validated and compiled through the same path. Selecting a preset resets its prefix to `C-b` or no
  prefix respectively, and `input.prefix` in the same table can override it.
- `input.prefix` is any valid key chord, or `false` for no prefix. A configured prefix enters the
  one-shot `prefix` context. Direct bindings in `normal` do not require a prefix.
- `terminal.scrollback_lines` is a nonnegative integer up to 10,000,000, or `false` for the native
  memory-bounded default.
- `terminal.clipboard_read` and `terminal.clipboard_write` are Boolean and default to `false`.
  They allow applications in the attached Session's active, focused Pane to request text or image
  clipboard access through the outer terminal's [native protocol](usage.md#application-clipboard).
  They do not bypass its consent policy. A live reload revokes incompatible pending requests;
  detach or focus loss also cancels
  them. Lemma stores no clipboard cache or remembered grants. Explicit selection copy and
  [`paste-image`](usage.md#clipboard-images) are separate user-authorized operations.
- `ui.status_line` enables or disables the shipped statusline extension. Disabling it releases its
  docked row and makes command-line and copy-search bindings inert. Replace the statusline through
  [managed extensions](extensions.md#managed-programs) to retain those native editors with custom UI.
- `ui.outer_title` is Boolean and defaults to `true`. It enables the outer
  [window title](usage.md#window-title).
- `ui.outer_notifications`, `ui.outer_progress`, and `ui.outer_cwd` are Boolean and default to
  `true`. They forward desktop notifications, progress, and the working directory to the outer
  terminal as described in [Attention and directory](usage.md#attention-and-directory).
- `launch.default_cwd` is empty or an absolute path. It applies when creation does not specify
  `--cwd` and, for a split or new Tab, the source Pane has no usable
  [OSC 7 directory](usage.md#sessions-tabs-and-panes).
- `launch.default_program` is an exact argv array, not a shell command. It is bounded to 64
  arguments and 4 KiB including terminators. An empty array selects the account login shell.
  Explicit creation arguments after `--` take precedence.
- `history.file` is empty by default, which keeps command history in memory only. An absolute path
  enables best-effort loading at daemon startup and atomic saving on clean shutdown. The parent
  directory must already exist; malformed, oversized, missing, or inaccessible files load as empty.
  Missing files may be created, but failed reads and malformed existing files are not replaced on
  shutdown. Loaded history seeds new Attachments; live Attachment histories diverge independently.

The shipped bindings live in [Lua policy](../src/user/keymap.lua), compiled at build time into
native lookup tables. [Default extension declarations](../src/user/defaults.lua) run before your
`init.lua`; `C-b s` invokes `session.manager`. Your keymap overrides and `input.preset = "none"`
apply afterward. Ordinary input never evaluates Lua.

`lemma.keymap.set(CONTEXT, KEY, ACTION[, DISPOSITION])` replaces or adds one binding. `ACTION`
may be a command string or one of these native-policy descriptors:

```lua
lemma.context.push("resize")                   -- push another routing context
lemma.context.push("prefix", { defer = true })  -- retain trigger bytes for replay
lemma.context.pop()                             -- pop the current transient context
lemma.keymap.replay()                           -- replay a deferred trigger
lemma.keymap.send("Home")                       -- encode a physical key for the pane
```

`DISPOSITION` applies to command strings: `"retain"` is the default and `"base"` returns from
transient contexts before invocation. `lemma.keymap.del(CONTEXT, KEY)` removes one seeded or
previously configured binding. Repeated declarations use the last declaration.

`lemma.context.push()` and `lemma.context.pop()` change routing state only. Semantic interactions
still use commands such as `enter_copy_mode`, `copy_leave`, `begin_rename_tab`,
`begin_command_line`, and `command_line_cancel` so Core remains authoritative for their state and
invariants.

`lemma.context.set(CONTEXT, OPTIONS)` configures `label`, `lifetime` (`"persistent"` or
`"one_shot"`), unbound behavior (`"forward"`, `"consume"`, `"replay"`, or `"retry"`), and whether
the context `preempts` another Attachment interaction. A nonempty active label replaces the normal
Session/Tab status row as flat text; callers need not add badge padding. `retry` leaves a one-shot
context and routes the unmatched key through its base context.

The bounded routing contexts are:

- `normal`: ordinary pane input;
- `prefix`: the conventional one-shot command context;
- `resize`: resize policy;
- `copy`: copy navigation and selection;
- `copy_go`: the default `g` one-shot grammar;
- `copy_search`: editable search query input;
- `copy_searching`: an in-progress bounded search;
- `rename`: session and tab prompt editing;
- `command_line`: interactive command-line editing, completion, and history;
- `messages`: read-only navigation of the bounded Attachment message log.

Core owns the commands and interaction state, but not their keys. Resize transitions, copy
navigation/search, rename editing, prefix replay, and pane key rewrites all come from the compiled
configuration. An explicit `preset = "none"` configuration can recreate the complete shipped
policy. After publication the router cannot distinguish a seeded binding from a user binding.

Key names use printable ASCII or structured names. Modifiers are `C-`, `S-`, `M-`/`A-`, and
`Super-`/`Cmd-`/`Command-`/`Win-`/`D-`. Named keys are `Space`, `Enter`, `Tab`, `Backspace`,
`Escape`, `Up`, `Down`, `Left`, `Right`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`,
and `F1` through `F12`. Super/Cmd and other non-terminal chords work for structured keyboard
clients; legacy byte-stream terminals cannot report chords they do not encode.

Keymap command names below are checked against the native catalog in
[config.cpp](../src/config/config.cpp). These are binding actions, not Proc command paths:

```text catalog=keymap-commands
detach
split_left_right split_top_bottom
resize_left resize_right resize_up resize_down
focus_left focus_right focus_up focus_down focus_next focus_previous
close_pane toggle_zoom
enter_copy_mode enter_copy_search_forward enter_copy_search_backward copy_selection
copy_cancel_or_leave copy_leave copy_cancel_selection
copy_move_left copy_move_down copy_move_up copy_move_right
copy_word_left copy_word_right copy_word_end
copy_line_start copy_line_first_nonblank copy_line_end
copy_history_top copy_history_bottom
copy_viewport_top copy_viewport_middle copy_viewport_bottom
copy_half_page_up copy_half_page_down copy_page_up copy_page_down
copy_visual_character copy_visual_line copy_visual_block copy_swap_endpoint
copy_repeat_search copy_reverse_search copy_cancel_search copy_commit_search copy_query_backspace
rename_cancel rename_commit rename_backspace rename_delete
rename_cursor_left rename_cursor_right rename_cursor_home rename_cursor_end
rename_clear rename_delete_word
create_tab next_tab previous_tab close_tab
begin_rename_session begin_rename_tab begin_command_line move_tab_left move_tab_right
show_messages message_view_leave message_view_previous message_view_next
message_view_page_previous message_view_page_next message_view_history_start message_view_history_end
command_line_cancel command_line_commit command_line_complete
command_line_history_previous command_line_history_next
command_line_backspace command_line_delete command_line_cursor_left command_line_cursor_right
command_line_cursor_home command_line_cursor_end command_line_clear command_line_delete_word
swap_pane_left swap_pane_right swap_pane_up swap_pane_down
select_tab_0 select_tab_1 select_tab_2 select_tab_3 select_tab_4
select_tab_5 select_tab_6 select_tab_7 select_tab_8 select_tab_9
```

## Custom commands

Register commands during configuration loading, directly in `init.lua` or a required module.
This [complete example](../examples/command.lua) opens a shell Tab:

```lua example=../examples/command.lua
local lemma = require("lemma")

lemma.command.register("work.shell", {
  description = "Open a shell tab",
  timeout_ms = 30000,
  handler = function(ctx, args)
    local result = ctx:proc({
      commands = {
        {
          command = "tab.new",
          session = { id = ctx.session },
          title = args[1] or "shell",
          argv = { "/bin/sh", "-l" },
        },
      },
    })
    assert(result.ok, "Could not open shell tab")
  end,
})
```

Open `C-b :`, type `work.sh`, press Tab to complete `work.shell`, then Enter. Arguments use the same
literal quoting grammar as native commands: `work.shell 'my shell'` passes one string. A registered
command name is also a keymap action: after registering it, use
`lemma.keymap.set("prefix", "s", "work.shell")`. Keybindings invoke with no arguments and capture
context when the key is routed, not when the host later runs it. They work with the status line
disabled; only the interactive command line requires that line. Registered commands are not public
Proc commands. Register a command before referring to it in a keymap.

`lemma.command.register(NAME, OPTIONS)` requires `description` and exactly one of a function
`handler(ctx, args)` or an `argv` array for an external program. `timeout_ms` is optional. Unknown options, duplicate names, or invalid declarations reject the
entire startup transaction, including configuration. Registration is not allowed from callbacks.
Names must be qualified, such as `work.shell`: dot-separated segments begin with a lowercase ASCII
letter and otherwise contain lowercase letters, digits, `_`, or `-`. This keeps extension commands
separate from native command roots.

`ctx.session`, `ctx.tab`, `ctx.pane`, and `ctx.connection` are generational ID strings captured at
invocation. `ctx.endpoint` is the current daemon's Unix socket path. Wrap them
in `{ id = ctx.pane }` selectors as in the public API. They do not follow later focus changes, and
normal stale-target checks apply. `args` is a one-based array of literal strings.

`ctx:proc(DOCUMENT)` yields the callback until the daemon returns the complete
`lemma.proc-result/v1` table. `schema = "lemma.proc/v1"` is supplied when omitted; all other fields,
validation, backward references, ordering, partial completion, and `on_error` behavior are those of
the [Automation API](api.md). Rejections are returned as results rather than thrown. A callback can
inspect the result and submit another Proc. Returning completes the command; throwing publishes an
error in the attachment's bounded message log and status row.

Lua strings, integers, booleans, and ordinary tables represent JSON values. Contiguous, nonempty
one-based tables become arrays; string-keyed and empty tables become objects. Omit empty optional
arrays. Cycles, mixed or sparse tables, functions, and noninteger numbers cannot be submitted.
JSON null results become Lua nil. Do not call `coroutine.yield` directly; use `ctx:proc`.

### External command programs

An `argv` declaration launches a fresh program in the isolated host on each invocation, without a
shell. Invocation arguments are appended literally. The program inherits the host environment and
working directory; executable lookup uses its `PATH`. The combined argument vector is bounded to
256 entries and 65,535 bytes, including NUL terminators. `stdin` is `/dev/null`. `stdout` and `stderr`
share a diagnostic pipe: more than 4 KiB terminates the child, and failures publish at most 180
printable diagnostic bytes. UI belongs on Surfaces, not stdout.

`LEMMA_COMMAND_CONTEXT` contains a `lemma.command-context/v1` JSON object with `command`, `args`,
`endpoint`, `session`, `tab`, `pane`, and `connection`. The IDs and endpoint are the captured values
above. Connect to that endpoint using the [runtime extension protocol](extensions.md), not a guessed
production socket path. Program exit completes the invocation; a nonzero exit reports an error.
External programs do not block Lua callbacks or each other while waiting for input. Reinvoking after
a child crash launches a fresh child without restarting the host.

These programs are invocation-scoped, not persistent extension services. Detach, disconnect, or
Session switching kills the direct child and retains its invocation slot until exit is acknowledged.
Programs must not daemonize or escape the host's process group. Host exit or the native watchdog
revokes that group. As with Lua callbacks, a deadline expires the whole host, not just one command.
A program's runtime connection is a separate owner: its Procs and Surfaces are revoked when the daemon
observes that connection closing, not synchronously when host cancellation is requested. Effects
already committed before closure are not rolled back.
For a complete keybound program, see the [navigation picker](extensions.md#navigation-picker).

Runtime bounds are:

- 64 registered commands, with names up to 64 bytes and printable ASCII descriptions up to 256 bytes;
- eight concurrent invocations; a Lua callback has at most one outstanding Proc, while an external
  program's runtime connection has the separate bounds in [Runtime extensions](extensions.md);
- one pending keybound invocation per Attachment per service turn; another reports capacity instead
  of growing a queue;
- a default 30-second invocation deadline, configurable from 1 to 600,000 milliseconds, including
  time spent waiting for Procs;
- one million Lua instructions per coroutine resume, plus the host-wide Lua allocation bound;
- 1 MiB per private runtime record, including its envelope, with a 2 MiB outgoing queue per peer;
- one 16 KiB read and write attempt and at most one record decode per reactor turn.

Callbacks share one host and cooperate by yielding through `ctx:proc`. A blocked native Lua call
can delay other callbacks, but not ordinary pane input, PTY progress, or rendering. The daemon
terminates the host on an invocation deadline. These bounds are not an OS security sandbox or an
aggregate CPU quota; only load trusted code.

## Failure and lifetime

The daemon owns the compiled configuration after publication. Lua remains in its separate host,
never on the input or render call stack. Only explicitly invoked custom commands require runtime
host communication. The private host protocol does not change the public lock-step CONTROL API.

An invocation belongs to the originating attachment generation. Detach, disconnect, or switching
Sessions cancels its remaining Lua-submitted Proc commands before further execution; external
programs use the asynchronous termination and connection-closure rules above. Completed effects are not
rolled back. Cancellation retains its bounded slot and deadline until the host acknowledges it, so
a blocked callback cannot evade the watchdog by detaching.

A callback error, instruction-budget failure, or external child exit ends that invocation without
disabling other commands. Host crash, protocol failure, or deadline expiration removes custom command discovery and
cancels outstanding invocations. Native bindings, compiled settings, Sessions, and ordinary pane
processes remain usable. [Explicit reload](#reload) can replace the failed command host without
restarting pane processes.
Closing the daemon's private lease closes the host. Invalid startup configuration never partially
changes the native map.
