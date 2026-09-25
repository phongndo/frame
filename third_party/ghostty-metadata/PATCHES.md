# Local Ghostty patch ledger

Production Ghostty commit: [`b0c421fcd2e290629d4285c181b52fe2f2095f06`](PIN.json).

The upstream submodule remains clean and exactly matches `PIN.json`. Both Nix and non-Nix builds
apply the ordered, SHA-256-verified patches in `PIN.json` to a private build-tree copy. The source
revision and patch contents identify the Ghostty build cache; neither the submodule nor the Nix
store source is modified.

## Resize viewport normalization

- Patch: [`patches/resize-viewport.patch`](patches/resize-viewport.patch).
- Affected upstream commit: `b0c421fcd2e290629d4285c181b52fe2f2095f06`.
- Upstream tracking: pending. The maintainer explicitly requested keeping this fix local rather
  than publishing an issue; this is an exception to the issue-URL requirement below.
- Reason: historical viewports can violate `PageList`'s viewport-pin integrity check during
  intermediate resize growth/reflow. Lemma cannot retain its parser-injected screen-switching
  workaround because it corrupts fragmented child CSI, OSC, and UTF-8 input.
- Behavior: `PageList.resize` temporarily uses the active viewport, then restores a historical
  viewport's bounded absolute row. This preserves Lemma's existing resize viewport policy without
  switching screens or changing VT parser state. Restoration is scoped to the owning page list,
  including error returns; no borrowed screen pointer survives screen replacement.
- Regression coverage: `TerminalResizeRegressionTest` covers historical primary viewports,
  historical primary viewports while alternate is active, and fragmented child input across resize.
- Owner: Lemma terminal maintainers.
- Removal condition: upstream resize safely maintains viewport-pin invariants throughout growth
  and reflow, and the regression tests pass without the patch and without adapter normalization.
- Revalidated pin: `b0c421fcd2e290629d4285c181b52fe2f2095f06`.

## Deferred clipboard replies

- Patch: [`patches/async-clipboard.patch`](patches/async-clipboard.patch).
- Affected/revalidated pin: `b0c421fcd2e290629d4285c181b52fe2f2095f06`.
- Upstream tracking: pending; the maintainer explicitly approved a local patch without upstream
  tracking for nonblocking image clipboard support.
- Reason: synchronous clipboard replies otherwise require blocking PTY parsing while a client
  reads its system clipboard. Clipboard helpers and consent must not suspend the daemon reactor.
- Behavior: additive C functions retain a clipboard request and its protocol reply state, suppress
  the synchronous default denial, and allow one later reply on the terminal-owning thread.
  Original synchronous callbacks are unchanged. Retention failure leaves the original request
  available for an immediate failure response. Retained requests must be freed before terminal
  reset/destruction; freeing without replying cancels them. Kitty write transactions release their
  payload state when deferred, independently of later writes. MIME data and request IDs are copied.
  Read/write request structs retain an `osc52` flag so the bridge preserves the original wire
  protocol rather than converting legacy text requests into unsupported MIME requests.
- Owner: Lemma terminal maintainers.
- Removal condition: upstream provides nonblocking, lifetime-safe clipboard completion, and
  Lemma's clipboard lifetime/protocol regressions pass against that interface without this patch.

## Native graphics projection and animation

- Patch: [`patches/kitty-render.patch`](patches/kitty-render.patch).
- Affected/revalidated pin: `b0c421fcd2e290629d4285c181b52fe2f2095f06`.
- Upstream tracking: maintainer explicitly approved extending the local-patch exception to
  Unicode-placeholder images and animation timing, without an upstream issue/PR.
- Reason: the C API exposes direct placements but not placeholder projection or native animation
  scheduling. Reimplementing these semantics in Lemma would create a second VT authority.
- Behavior: additive C functions project native placements, Unicode placeholder runs, and parent
  chains into caller-owned bounded storage; another advances native animations on the caller's
  monotonic clock and reports the next delay. Pixels stay in canonical native storage; no borrowed
  pointer survives terminal mutation. Projection is capped at 1024 stored images/placements and
  16384 lookup/run operations. No file/shared-memory capabilities or snapshot formats change.
- Regression coverage: terminal-boundary graphics tests exercise placeholder geometry, relative
  placement, deterministic animation ticks, and composition/lifetime behavior.
- Owner: Lemma terminal maintainers.
- Removal condition: upstream exposes native placeholder projection and animation deadlines, and
  Lemma's graphics regressions pass using those interfaces without the patch.

## Semantic prompt command transitions

- Patch: [`patches/semantic-prompt.patch`](patches/semantic-prompt.patch).
- Affected/revalidated pin: `b0c421fcd2e290629d4285c181b52fe2f2095f06`.
- Upstream tracking: none. The maintainer explicitly approved this local patch without an upstream
  issue or PR.
- Reason: the C API exposes OSC 133 only as per-row/cursor prompt state. Command start and the
  OSC 133;D exit status, which Ghostty already parses, are otherwise discarded, and Lemma must not
  reparse PTY bytes as a second VT authority to expose them as Pane signals.
- Behavior: an additive `ghostty_terminal_set_semantic_prompt_callback` installs a callback that
  receives each parsed OSC 133 action and its optional exit code after Ghostty applies the command
  to terminal state, including when that application fails. Terminal state, parser behavior,
  child-visible capabilities, allocation, and existing callbacks are unchanged; no option enum value
  is added.
- Regression coverage: `TerminalTest.ReportsSemanticPromptCommandStateThroughLocalHook`,
  `TerminalTest.CountsOneCompletionPerShellIntegrationCommand`, and the extension-runtime
  `pane.signal` mux test.
- Owner: Lemma terminal maintainers.
- Removal condition: upstream exposes semantic prompt command transitions with exit status through
  the C API, and the regressions pass using that interface without this patch.

## Requirements for future patches

Record each patch before applying it, including the affected commit and patch file, upstream issue
or PR URL (or an explicit maintainer-approved exception), reason Lemma cannot wait, behavior and
regression tests, owner, removal condition, and the upgrade where it was revalidated.

A patch may not silently change child-visible capabilities, snapshot compatibility, allocator
behavior, or a public `libghostty-vt` result/mode/effect contract.
