# frame — MVP Specification

## Vision (MVP Scope)

`frame` is a **terminal-first AI review IDE**.

It replaces:

* manual diff reading
* copy-pasting feedback into AI chats

With:

* code-first navigation (like Helix)
* inline change visualization (not raw diffs)
* inline comments on real code
* one-key feedback loop to AI CLI

**frame does NOT edit code.**
It supervises AI-generated changes.

---

## Core Philosophy

> Reviewing is the mountain we hold.

Everything in `frame` is optimized for:

```id="philosophy"
understanding code changes quickly and confidently
```

The user should feel like they are **reading code**, not inspecting patches.

If a feature does not improve review clarity or speed, it does not belong in MVP.

---

## Entry UX (Critical)

```bash id="entry"
frame
```

* detects git repo
* loads changes
* opens UI immediately

No commands. No friction.

---

## Core Interaction Model

`frame` operates in **code-first mode with change overlays**.

Instead of rendering raw diffs:

```text
git diff → parse → overlay changes on full file
```

The user sees:

* full file context
* highlighted additions/modifications
* optional ghosted deletions

Diffs are **data**, not the UI.

---

## Navigation Philosophy (Critical)

Navigation behaves like **Helix/Vim**:

* free movement across the file buffer
* no restriction to hunks or diff blocks
* smooth scrolling like an editor

### Reorientation Signals

Because context is continuous:

* clear file headers
* highlighted changed lines
* optional subtle markers for change boundaries

### Reorientation Movements

```id="nav_reorient"
ctrl-d / ctrl-u
             half-page down/up
]c / [c      next/prev change
]f / [f      next/prev changed file
gg / G       start/end
```

Goal:

> allow the user to explore freely, but recover instantly

---

## Change Visualization (Critical)

Changes are rendered as **overlays on real code**.

### Types

* added lines → highlighted
* modified lines → emphasized
* removed lines → ghosted or inline (optional)

### Requirements

* preserves full code context
* no broken indentation
* no visual distortion
* changes are visible but not overwhelming

### Principle

> show the code first, show the change second

---

## Syntax Highlighting (Critical)

Rendering must feel like a real editor:

* language-aware highlighting
* consistent with nvim / helix
* supports common languages (C++, Rust, Python, TS)

Non-goals (MVP):

* no LSP
* no semantic analysis

---

## Diff Mode (Secondary)

Raw diff view exists as a **secondary mode**, not primary.

Used for:

* edge cases
* precise patch inspection

Navigation:

```id="diff_nav"
]h / [h      next/prev hunk
```

---

## Core User Flow

1. User runs:

   ```bash
   frame
   ```

2. AI modifies repository

3. frame:

   * loads changed files
   * renders code with change overlays

4. User:

   * navigates code
   * reviews changes in context
   * adds comments

5. User presses:

   ```
   A
   ```

6. AI revises patch

7. frame reloads changes

Loop repeats.

---

## MVP Features

### 1. Code Viewer (Critical)

* load full file contents
* render as editor-like buffer
* overlay changes from git diff

Navigation:

```id="nav_basic"
j / k        move
ctrl-d / ctrl-u
             half-page down/up
]c / [c      next/prev change
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

* attach comment to current code line
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
* `symbol` is best-effort
* cleared after successful AI send + refresh

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

After AI completes:

* re-run `git diff`
* reload overlays and files

No manual refresh in MVP.

---

### 6. File List Panel

Shows changed files:

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
ctrl-d / ctrl-u
             half-page down/up
]c / [c      next/prev change
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
* no full repo explorer (only changed files)
* no full git UI

---

## Definition of Done

frame is usable when:

* user reads real code, not diff blobs
* changes are clear but not overwhelming
* navigation feels like an editor
* review → comment → AI loop is seamless

---

## One-Sentence Summary

frame = **a terminal-first AI review IDE**
