# Architecture

Lemma's main C++23 executable has client, daemon, and control roles; the separate `lemma-ui`
executable supplies the shipped user interface. One per-user daemon owns all live mux and terminal
state. Clients are replaceable input and presentation edges.

```text
Lua config -> isolated host -> validated draft -> immutable native generation
                                                       |
physical input -> compiled input policy ----------------+
CLI / API / mouse -> typed command -> Core -> Runtime -> PTY/process
extension -> Proc / SurfaceUpdate -------------|
PTY output -> Ghostty terminal -> retained Scene -> client
```

This document owns component boundaries and data flow. User behavior is in [Usage](usage.md),
execution semantics in [Automation API](api.md), and external UI contracts in
[Extensions](extensions.md).

## Model and lifetimes

The kernel hierarchy is `Session -> Tab -> Pane`:

- A **Session** owns launch context, ordered Tabs, identity, lifecycle, and attachment policy.
- A **Tab** owns its tiled pane layout, float layer, focus, zoom, ordering, and title policy.
- A **Pane** is the semantic identity of one process surface.
- An **Attachment** is the controller's view and interaction state for a Session.

Projects, worktrees, tasks, and agent runs compose stable IDs through the public API rather than
becoming kernel objects.

Semantic identities have different lifetimes from external resources:

```text
Session != Attachment != AttachmentRuntime
Pane    != PaneRuntime
```

`PaneRuntime` owns the child process, PTY, terminal, write queue, and scheduling state.
`AttachmentRuntime` owns the replaceable client connection, decoder, retained output progress,
presentation caches, and deadlines. Losing it detaches the client; it does not destroy the Session
or semantic Attachment. Stable IDs cross boundaries; borrowed references remain owner-local.

## Core and user layers

The architectural split is between native mux mechanisms and user-layer behavior. The native layer
spans Core, Runtime, Input, Terminal, and Scene; it is broader than the `lemma_core` build target.
It owns Session/Tab/Pane semantics, processes and PTYs, terminal state, layout, input routing, frame
scheduling, composition, and bounded extension admission and cleanup. Extensions compose these
mechanisms through Procs, Events, and Surfaces. Extend the native interface when a demonstrated mux
capability requires it; keep workflow-specific state and policy in the user layer.

Statusline content and interaction, session-manager presentation, navigation, and project or agent
workflows belong in the user layer. Shipped UI should be replaceable first-party extensions using
the same public interface as user-installed extensions. A session manager chooses what to display
and which Commands to submit; native code remains authoritative for creating, destroying, attaching,
and switching Sessions. A statusline supplies retained content; native code reserves its space and
composes it with Panes. Distribution as a default extension does not grant privileged access to Core.

Lua declares keybindings and command associations, which compile into native routing policy.
Matching a binding and forwarding ordinary terminal input must not wait for Lua or an external
process. Extension commands execute asynchronously. Keep native recovery sufficient to revoke
extension input ownership and regain control independently of extension execution.

The split must preserve terminal responsiveness: native code composes retained extension state
without callbacks or a synchronous dependency on extension progress. Drive user-layer updates
from bounded observations and changed content rather than terminal-byte or frame callbacks. Idle
extensions should sleep; terminal screen projections remain opt-in. Validate native and helper
resource costs with the [performance requirements](performance.md), including the extension-isolation
gate when changing this seam.

The shipped `lemma-ui` process supplies the statusline and session-manager UI through public
Events, Procs, and Surfaces. Native command editing, copy/search state, completion, and recovery
remain authoritative state machines; their prompt representation is observed and rendered by the
statusline. Managed extension process groups have bounded restart independently of the Lua command
host. The [extension contract](extensions.md) defines lifecycle, observation, and UI capabilities.

## Components

[Build targets](../CMakeLists.txt) define the dependency boundaries:

| Component | Responsibility |
| --- | --- |
| `lemma_app` | CLI grammar and executable role selection |
| `lemma_daemon` | Endpoint ownership, connection admission, and reactor |
| `lemma_api` | Public Proc, Command, Event, JSON, and schema values |
| `lemma_core` | Session/Tab/Pane semantics, commands, layout, and copy policy |
| `lemma_input` | Compiled physical keymaps and per-Attachment routing contexts |
| `lemma_config` | Configuration values, validation, and native generation compilation |
| `lemma_extension_contract` | Lua command declarations and language-neutral extension protocol |
| `lemma_extension` | Isolated command host, Lua callbacks, managed programs, external children, and configuration admission |
| `lemma_extension_client` | Public framed client used only by external user programs |
| `lemma-ui` | Replaceable first-party statusline and session-manager UI |
| `lemma_runtime` | Extension generations/Surfaces, processes, PTYs, scheduling, input, resize, and frame progress |
| `lemma_terminal` | The only boundary allowed to include or link against libghostty-vt |
| `lemma_render` | Non-authoritative pane and frame presentation |
| `lemma_status` | Pure status-row projection and UI cells, without terminal-runtime dependencies |
| `lemma_protocol` | Bounded private attachment codec |
| `lemma_client` | Host input, outer-terminal presentation, and restoration |
| `lemma_platform` | OS I/O, PTYs, and terminal mode mechanisms |

Core links no Lua VM, PTY, socket, process, or terminal-emulator owner. Runtime executes accepted
semantic intent using those mechanisms. Ghostty representations remain private to `lemma_terminal`;
Lemma-facing types and borrowed views make lifetimes explicit.

## Authority

| Mutable state | Authoritative owner |
| --- | --- |
| Sessions, Tabs, Panes, layout, float layers, focus, zoom, stable IDs | Core |
| Attachment view, copy/editor state, command history, and message log | Core |
| Key bindings, context options, transitions, and transient routing state | Input policy |
| Lua VM, coroutine state, and uncommitted configuration draft | Isolated host process |
| Processes, PTYs, descriptors, polling, clocks | Runtime |
| Canonical screen, history, modes, cursor, selection primitives | Ghostty behind `vt::Terminal` |
| Connection decoding, output progress, and transient frame/message deadlines | AttachmentRuntime |
| Admitted Proc execution, waits, and owner-generation cancellation | Reactor Proc table |
| Hosted command invocation, captured targets, deadline, and attachment-generation owner | Reactor-owned command runtime |
| Extension capabilities, generations, Surface IDs, retained Grids, and Surface focus | ExtensionRuntime |
| Frame buffers, Scene composition, damage, and physical presentation shadow | Scene/render runtime |

Presentation caches are bounded, invalidatable projections, reconstructible from their owners.
Extension aggregate resource accounting is likewise derived from peer-owned transport and reservation
state, not a competing mutable ledger. Slow peers cannot prevent unrelated PTY progress; queues and
per-turn work have explicit bounds.

On Linux, the reactor retains level-triggered epoll registration for stable descriptor lifetimes;
other platforms use poll. Watches derive their identity from existing owner generations and are
rebuilt on membership or lifetime changes, including descriptor-number reuse. Readiness never
changes dispatch order or byte budgets. Unsupported descriptors fall back to poll.

## Mutation and configuration

Every semantic mutation uses a typed command and result. CLI syntax and backward result references
are frontend representations, not Core state. Proc admission validates the complete request once;
execution resolves concrete generational IDs and checks lifetime and ownership immediately before
each Command. Closing an owner revokes its remaining work.

Lifecycle transitions go through [`SessionMachine`](../src/core/session_machine.cpp). Core stages
fallible semantic owners, Runtime executes a bounded spawn/resize/retire effect batch, and Core
publishes only after required effects succeed. Rejected effects do not publish partial Core state.
Events observe committed state and never provide a second mutation path.

The interactive command line is another typed frontend using the same executor. One native catalog
owns command paths and completion metadata. A Session switch transfers a drained connection decoder
and sequence, then forces a full redraw; it does not create a nested client. The external statusline
observes editor state and supplies a retained Grid. Production frame composition receives that Grid,
without calling a status renderer or invoking extension code.

The daemon borrows one immutable compiled configuration generation. Shipped and user-declared input
policies compile through the same path: Core owns semantic commands; configuration chooses their keys
and routing transitions. Ordinary input, PTY processing, and composition never call into Lua.
Explicit reload stages a separate host, consumes its bounded registration asynchronously, and
replaces the generation between reactor stages after cancelling old invocation ownership. All input
routers switch before the old generation is released; child reaping retains revoked process-group
identity until cleanup. [Configuration](configuration.md#reload) owns reloadable settings and
interaction/failure semantics.

Keybindings compile hosted names to bounded command indices. Routing captures invocation context and
queues at most one invocation per Attachment; host service, not the input stack, launches it. The
isolated host runs either a Lua coroutine or an argv-declared external child with bounded diagnostics.
External children present UI through the ordinary extension endpoint and remain invocation-scoped.
Connection generations are owned by the daemon's Session store, surviving Session slot reuse.

Explicit hosted commands communicate asynchronously with the isolated host. Lua callbacks yield Procs
through `ctx:proc`; those use ordinary validation, admission, and round-robin execution. Their typed
completion owner is the originating attachment generation, revoked on detach or Session switch.
Cancellation keeps the invocation slot and deadline until acknowledgement so a blocked callback cannot
escape its watchdog. Host failure removes hosted commands without changing compiled input policy or
pane lifetime. See [Configuration](configuration.md#failure-and-lifetime) for the public behavior.

## Terminal and presentation flow

Ghostty owns VT semantics. Lemma owns process, mux, security, scheduling, and presentation policy.
PTY bytes are parsed once into the Pane's canonical terminal:

```text
PTY -> Ghostty parse
          |-> terminal responses -> ordered PTY write queue
          |-> effects -> Lemma policy
          +-> damage -> retained Scene composition -> attached client
```

Application attention effects (BEL, OSC 9/777 notifications, OSC 9;4 progress, OSC 133 command
state, and title/directory changes) update a fixed per-terminal latest-value record with O(1)
stores; the drain only stamps the Pane and Session as changed. Observers read that record through
[Pane signals](api.md#pane-signals). Frame composition forwards it to the attached client's outer
terminal, retaining only the connection's presented values and pacing
([Usage](usage.md#attention-and-directory)).

Terminal responses enter the ordered write queue before later accepted application input.
Mode-dependent keyboard, paste, focus, and mouse encoding comes from the target Pane's Ghostty
terminal. Attach, resize, tab changes, and lag recovery can reconstruct a full ANSI frame from
daemon-owned state; the client owns neither a second terminal grid nor a PTY replay log.

Scene composes Pane projections and extension-owned retained Grids. Native code owns clipping,
occlusion repair, damage, and cursor arbitration. Extension code never enters composition.
Panes omitted from a full frame release their render rows and physical presentation shadow, so
hidden Panes keep only canonical terminal state; presenting one again rebuilds them with full damage.

```text
Attachment geometry -> Surface placement -> Core layout -> Pane geometry -> PTY size -> Ghostty size
```

Docks change the effective pane viewport; float and overlay Surfaces do not. Floating Panes resolve
their placement against that viewport, and their PTYs resize in the same effect batch as the tiled
layout. Focus is one derived value: the Tab's top float while its float layer holds focus, else the
tiled focus. The child PTY receives target dimensions before Ghostty parses output at those
dimensions. Multi-pane resize publishes semantic geometry only after dependent runtime work
succeeds. Attached clients also report cell pixel size; that geometry follows the connection
during Session transfer and participates in native resize.

The client sends an outer-terminal size change immediately. While a window drag continues, it
coalesces SIGWINCH and in-band size reports (mode 2048) into at most one geometry per 16 ms display
interval; the final size follows the last change by at most one interval. Once observed, in-band
reports take precedence over the PTY size ioctl. Reflow cost grows with history, so geometry can
still queue at the daemon. Consecutive queued geometry messages supersede one another, and the
daemon applies only the newest, as one budgeted step per reactor turn. Pending geometry is sent
and applied before any later input from the same client.

Kitty image pixels, placements, placeholders, and animation frames stay in Ghostty-owned canonical
state. The terminal adapter exposes bounded borrowed projections using the
[local native hooks](../third_party/ghostty-metadata/PATCHES.md). An Attachment-owned graphics cache
retains only upload progress, image generations, and presentation geometry; all pixel views end with
the composition call. Native code clips images against Panes and Surface coverage, namespaces outer
image IDs, and schedules bounded continuation frames and animation deadlines. No extension executes
in this path. File/shared-memory graphics transports remain disabled.

Application clipboard callbacks retain an owned native request rather than waiting for the client.
One correlated, deadline-bound transaction belongs to the Attachment. Recognized outer replies have
a dedicated input/protocol record and enter the originating Pane's ordered response queue, never
keymaps, Surface input, or paste. Once recognized, an incomplete OSC reply has a fixed 30-second
transport deadline, independent of the Escape-key disambiguation timer; timeout fails the attachment
closed rather than leaking partial clipboard data as input. Bracketed paste remains opaque.
Complete OSC records allow rendering
between clipboard chunks; cancellation aborts an unfinished write rather than committing partial
contents. Permission and focus are rechecked on service. Image-to-path paste retains Proc/connection
ownership while `lemma-clipboard-host` performs PNG validation and filesystem work outside the
reactor. [Usage](usage.md#clipboard-images) defines its user-visible file and permission semantics.
