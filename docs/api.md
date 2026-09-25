# Automation API

Lemma exposes one execution model and one observation model:

```text
Proc    one to 64 validated, ordered Commands
Event   immutable asynchronous observation
```

A Proc is the only execution request. It contains bounded Commands such as `pane.split`, and every
Command produces a nested `lemma.command-result/v1` result. Shell automation can use basic pane
commands with `--json`, resource commands, `lemma proc`, and `lemma events`. Dedicated clients use
the same JSON contracts over the per-user Unix endpoint.
Within this control API, `action` is reserved and defines no request, result, Proc member, or CLI
namespace in v1.

## Discover the installed contract

```sh
lemma api schema
lemma api schema --json
lemma proc DOMAIN COMMAND --help
```

The JSON form is the authoritative JSON Schema 2020-12 document for Commands, Procs, results,
subscriptions, and Events. It is embedded in the binary and requires no running daemon.

`lemma api schema` summarizes ordinary Commands; `--json` also includes the extension-only
Surface Commands, `lemma.extension/v1`, Surface updates, and interaction Events. See
[Runtime extensions](extensions.md) for the framed transport and ownership rules. Surface lifecycle
Commands are rejected outside an admitted extension generation because their owner and Attachment
scope come from that connection.

## One-Command Procs

The direct CLI forms build a one-Command `lemma.proc/v1` request:

```sh
lemma session start work --cwd "$PWD"
lemma tab new --session work --title tests --focus preserve -- just test
lemma split --json --session work --pane 0:1 --right --focus preserve
lemma send --json --session work --pane 0:1 --paste 'just test' --key enter
lemma capture --json --session work --pane 0:1 --source recent --lines 100
lemma wait --json --session work --pane 0:1 --timeout 30s
```

`split`, `send`, `wait`, `capture`, `focus`, `zoom`, `swap`, and `resize` are basic pane verbs.
They share targeting, validation, and execution with resource commands; `send` uses `pane.input`.
Without `--json`, they provide the [readable output described in Usage](usage.md#sessions-tabs-and-panes).
`lemma pane COMMAND` and `lemma proc pane COMMAND` retain canonical JSON output by default.
The existing text-only `pane send --text TEXT` spelling remains available; ordered text, paste,
and keys use `pane input` or the basic `send` verb.

Inside a Lemma pane, the CLI may infer omitted targets from `LEMMA_SESSION_ID`, `LEMMA_TAB_ID`, and
`LEMMA_PANE_ID`. This is a CLI convenience: the Command sent to the daemon always contains concrete
targets. Outside Lemma, provide the required Session and resource selectors.

Session names and one-based Tab positions are discovery conveniences. Persistent automation should
retain returned generational IDs. Tab and Pane IDs are Session-scoped. Pane listings expose PID
inside process metadata for lifetime observation only; PID is not a Pane selector or identity.

Resource commands, basic pane commands with `--json`, and `lemma proc DOMAIN COMMAND` print one
`lemma.proc-result/v1` value whose single `results` entry
wraps a `lemma.command-result/v1` result. Successful Command statuses are `applied` and `no_effect`.
Other statuses include `stale`, `wrong_owner`, `conflict`, `capacity`, `unavailable`, and `failed`.
Results include the relevant stable IDs and, where applicable, the current Session revision or
terminal generation.

`pane.input` admits one ordered batch of text, opaque paste, and logical key events. Logical keys use
an optional `phase` of `press`, `repeat`, or `release`; it defaults to `press`. `pane.capture` reads a
bounded visible, recent, or last-command projection. `pane.wait` is a finite Command;
without a condition it waits for child-process completion.

## Reloading configuration

`config.reload` stages a new daemon configuration and completes only after publication or rejection.
It accepts no fields beyond `command`. `lemma config reload` submits the same Command; the native
prompt spells it `reload`. See [Configuration](configuration.md#reload) for live settings, cancellation,
host replacement, and startup-only changes. Reload is daemon-scoped, not Session-scoped.

## Pasting a clipboard image

`pane.paste-image` accepts `session`, `pane`, and optional `if_session_revision`, like `pane.focus`:

```json
{"command":"pane.paste-image","session":{"name":"work"},"pane":{"id":"0:1"}}
```

CLI spellings are `lemma paste-image` and `lemma proc pane paste-image`. The Command asynchronously
reads a PNG through the attached client's outer terminal, validates/saves it on the daemon host,
and submits a structured paste containing its quoted path. Success returns `path`. The target must
remain focused on the same connection, and the requesting Proc must remain owned. This is an
explicit user action, independent of application clipboard grants. There is no automatic replay.
See [clipboard images](usage.md#clipboard-images) for file lifetime, limits, and terminal requirements.

Stable rejection reasons include `focused_attachment_required`, `revision_mismatch`, `clipboard_busy`,
`clipboard_owner_changed`, `clipboard_denied`, `clipboard_unavailable`, `clipboard_image_unavailable`,
`invalid_png_or_file_error`, and `invalid_file_path`. As with other Procs, already completed effects
are not rolled back; cancellation cannot undo clipboard operations already accepted by the outer
terminal or a file already saved.

## Switching an Attachment

`attachment.switch` transfers the one connected controller identified by `connection` to the selected
`session`. It is a JSON Command (use a Proc document; there is no dedicated CLI shorthand):

```json
{"command":"attachment.switch","connection":"0:3","session":{"id":"1:2"}}
```

Configured commands receive `connection` in their [captured invocation context](configuration.md#custom-commands).
It identifies a connection lifetime, not a Session name or OS PID. IDs are invalidated by detach and
transfer, and cannot address a replacement after Session slot reuse. Use the returned `connection`
after a successful transfer; switching to the same Session returns `no_effect` and the unchanged ID.

The operation never steals another controller. An attached or reserved target returns `conflict`
with reason `target_attached`; a stale source connection returns `stale` with reason
`stale_connection`. Pending source frame or clipboard output returns retryable `conflict` with
reason `output_pending`, without transferring anything. Retry that Command after output progresses,
not an already-executed Proc prefix. `if_session_revision` checks the destination Session.

Native code performs geometry, theme, connection, and input handoff through the same transition used
by the native Session switcher. Tab and Pane selection compose as preceding `tab.select` and
`pane.focus` Commands. As always, a Proc is ordered, not atomic. Switching cancels invocations tied
to the old connection; an invocation performing its own switch may exit before consuming its result.

## Multi-Command Procs

A Proc contains at most 64 Commands. This [runnable job](../examples/job.json) starts a held Pane,
waits for its process, captures output, and cleans up only its own Session. `on_error: continue`
keeps capture and cleanup reachable after an unexpected exit or timeout; still inspect every result.

```json example=../examples/job.json
{
  "schema": "lemma.proc/v1",
  "on_error": "continue",
  "commands": [
    {"id":"job", "command":"session.start", "name":"example-job",
     "hold":true, "argv":["echo", "hello from Lemma"]},
    {"command":"pane.wait", "pane":{"result":"job"},
     "exit_code":0, "timeout_ms":5000},
    {"command":"pane.capture", "pane":{"result":"job"},
     "source":"recent", "lines":20},
    {"command":"session.kill", "session":{"result":"job"}}
  ]
}
```

From the checkout:

```sh
lemma proc --file examples/job.json
lemma proc --stdin < examples/job.json
```

Before executing anything, the daemon validates and compiles the complete envelope, every Command,
all selectors, bounds, IDs, and backward-only result references. References may select the
`session`, `tab`, or `pane` field of an earlier creation result.

Commands then execute in document order through the reactor-owned Command executor. A Proc is not
atomic: completed process and PTY effects are not rolled back. `on_error` is `stop` by default or
`continue`. The `lemma.proc-result/v1` response contains one `lemma.command-result/v1` value per
executed Command and reports partial completion explicitly.

## Events

`lemma events` opens an observation stream:

```sh
lemma events --session work --pane 0:1
lemma events --session work --pane 0:1 --screen
```

A `lemma.events/v1` subscription selects one Session and optionally up to eight Panes. The stream
starts with an authoritative snapshot, then emits ordered state, process, terminal-invalidation,
optional screen, and closure Events. Screen data is opt-in, bounded current state rather than raw
PTY replay.

Observers cannot mutate state and are not terminal Attachments. A slow observer cannot block PTY
progress. Reconnecting creates a fresh snapshot; the daemon retains no Event replay log.

## Pane signals

Applications report attention through terminal sequences. Each Pane retains the latest value of
each signal, not a queue of occurrences:

| Field | Source | Value |
| --- | --- | --- |
| `bells` | BEL | Cumulative count |
| `notifications`, `notification` | OSC 9, OSC 777 `notify` | Count; latest `title`, `body`, and `truncated` |
| `progress` | OSC 9;4 | Latest `state` (`normal`, `error`, `indeterminate`, `paused`) and `percent`; null once removed |
| `commands`, `command` | OSC 133 shell integration | Completed-command count; latest `state` (`prompt`, `running`, `finished`) and `exit_code` |
| `title_changes`, `cwd_changes` | OSC 0/2, OSC 7 | Cumulative counts; `pane.inspect` reports current values |

Notification title and body share a 4 KiB bound (title at most 1 KiB); invalid UTF-8 and control
characters become `?`. A command completes only when OSC 133;D follows OSC 133;C (`running`); that
D increments `commands` and sets `exit_code`, which is null when the report omitted it. Shell
integrations also send D to close a prompt: fish sends a bare D after `D;$status`, bash repeats the
previous status after an empty Enter, and zsh sends a bare D. A D in any other state changes
nothing. Each Pane tracks one command state, not nesting: markers from the innermost shell
integration drive it. A nested shell started as a command (for example fish from bash) can end or
hide the outer command, since its first prompt's bare D arrives while the outer command is running. Counters saturate. `generation` increases whenever a field changes, including each
counted occurrence; an identical repeated progress report or prompt marker leaves it unchanged. It
is zero for a Pane that never reported a signal.

`pane.inspect` returns the complete record as `signals`; `pane.list` includes it without
`notification`. A subscription with `"signals": true` (`lemma events --signals`) adds `pane.signal`
Events for every Pane in its scope: the selected Session, all Sessions for a global feed, or only
the listed Panes. After the snapshot, each in-scope Pane with a nonzero generation is reported
once. Later changes coalesce: an observer that has not drained its output receives at most one
record per changed Pane, containing current values, never a backlog. When selected Panes also
produce terminal Events, the two kinds alternate, so a continuously changing Pane cannot withhold
another Pane's signals. Detecting a signal does no per-byte work, and unchanged Sessions cost an
observer no Pane scan.

`pane.wait` with `until_command: true` (`--until-command`) completes at the next command
completion after the wait starts, or after completion `after_commands` when supplied. When it
directly follows another Command in a Proc, it starts with that Command, so input sent by the
previous step cannot complete before the wait observes it. Its `completion` reports the `commands`
count and `exit_code` of the latest completion; if several complete within one terminal read, the
earlier ones are not reported separately. Process exit first is an `unexpected_exit`.
Close-on-exit Panes report the actual exit status to pending waits even though the Pane itself is
removed. [Usage](usage.md#sessions-tabs-and-panes) describes the readable CLI result.

## Direct connections

The public integration endpoint is `/tmp/lemma-UID.sock`, owned by the current user with owner-only
permissions. Public JSON records are compact UTF-8 values terminated by LF. Each record is bounded
to 1 MiB, 4,096 JSON values, and 32 levels.

A CONTROL connection accepts only `lemma.proc/v1` requests and is lock-step: send one Proc, read its
complete result, then send the next. There is no pipelining or request-ID layer. Closing the
connection cancels its admitted Proc before another Command executes. An OBSERVE connection begins
with an Events subscription and only receives Events. Terminal attachment uses a separate private
framed protocol on the same endpoint.
