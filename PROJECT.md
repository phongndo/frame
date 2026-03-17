# frame — MVP Specification

## Vision (MVP Scope)

`frame` is a **terminal-first AI patch reviewer**.

It replaces:

* manual diff reading
* copy-pasting feedback into AI chats

With:

* fast keyboard-driven diff navigation
* inline comments on patches
* one-key feedback loop to AI CLI

**frame does NOT edit code.**
It supervises AI-generated changes.

---

## Core Philosophy

> Reviewing is the mountain we hold.

Everything in `frame` is optimized for one thing:

```id="philosophy"
understanding code changes quickly and confidently
```

If a feature does not improve review clarity or speed, it does not belong in MVP.

---

## Entry UX (Critical)

```bash id="entry"
frame
```

* detects git repo
* loads diff
* opens UI immediately

No commands. No friction.

---

## Navigation Philosophy (Critical)

Diff behaves like a **Helix/Vim buffer**:

* free movement across entire diff
* no forced snapping to hunks or files

User may lose context, but must recover instantly.

### Reorientation Signals

* clear file headers
* strong hunk boundaries
* visible line origin:

  * `+` added
  * `-` removed
  * ` ` context

### Reorientation Movements

```id="nav_reorient"
]h / [h      next/prev hunk
]f / [f      next/prev file
gg / G       start/end
```

---

## Diff Rendering Quality (Critical)

Diffs must be **beautiful, readable, and information-dense**.

This is a primary feature, not polish.

### Requirements

* syntax-highlighted code (not plain text)
* visually distinct additions/removals
* clear indentation and alignment
* no visual noise or clutter

### Visual Goals

* easy to scan at high speed
* differences pop immediately
* structure is visible at a glance

### Styling Expectations

* additions: readable, not neon
* deletions: visible but not overwhelming
* context lines: slightly dimmed
* hunk headers: clearly separated

### Readability Principles

* prioritize contrast over decoration
* avoid excessive colors
* preserve code structure exactly
* never distort alignment

---

## Syntax Highlighting (Critical)

Diff view must feel like reading real code.

Requirements:

* language-aware highlighting
* consistent with modern editors (nvim / helix)
* supports common languages (C++, Rust, Python, TS)

Non-goals (MVP):

* no deep semantic highlighting
* no LSP-driven analysis

But:

> it should feel like an editor, not a pager

---

## Core User Flow

1. User runs:

   ```bash
   frame
   ```
2. AI has modified repo
3. frame loads diff
4. User:

   * navigates
   * comments
5. User presses:

   ```
   A
   ```
6. AI revises patch
7. frame refreshes

Loop repeats.

---

## MVP Features

### 1. Diff Viewer (Critical)

* parse `git diff`
* render:

  * file list (left)
  * diff buffer (main)

Navigation:

```id="nav_basic"
j / k        move
]h / [h      next/prev hunk
]f / [f      next/prev file
gg / G       top/bottom
```

---

### 2. Comment System (Critical)

Primary action:

```id="comment_key"
space
```

Behavior:

* attach comment to current line
* inline input

Data model:

```id="comment_model"
comment {
  file: string
  line: int
  side: enum { added, removed, context }
  symbol?: string
  text: string
}
```

Notes:

* comments are ephemeral
* `symbol` is best-effort and optional
* comments are cleared only after a successful AI send + refresh

---

### 3. Feedback Generator (Critical)

```id="feedback_key"
A
```

Prompt:

```id="feedback_prompt"
You previously generated a patch.

Review comments:

[file=path line=n side=added|removed|context symbol=optional]
comment text

Revise the patch accordingly.
```

---

### 4. AI CLI Integration (Critical)

Config:

```id="config_path"
~/.config/frame/config.toml
```

Execution:

```id="ai_exec"
spawn(command)
write(prompt → stdin)
close stdin
wait
```

Requirements:

* no shell
* exact byte preservation

---

### 5. Auto Refresh

After a successful AI run:

* re-run `git diff`
* reload UI automatically

No manual refresh key in MVP.

---

### 6. File List Panel

```id="file_list"
src/vector.cpp
src/matrix.cpp
```

---

### 7. Minimal Git Actions

```id="git_actions"
s → stage (toggle)
C → commit
```

---

## Keybindings Summary

```id="keymap"
j / k        move
]h / [h      next/prev hunk
]f / [f      next/prev file

space        comment
A            send to AI
s            stage (toggle)
C            commit
q            quit
```

---

## Tech Stack

* Rust
* ratatui
* git via subprocess
* AI via subprocess

---

## MVP Constraints

* no comment persistence
* no LSP
* no semantic diff
* no modes
* no full git UI

---

## Definition of Done

frame is usable when:

* diff is visually clear and fast to scan
* syntax highlighting feels like a real editor
* user can navigate without losing control
* review → comment → AI loop is seamless

---

## One Sentence Summary

frame = **lazygit for AI patch review**
