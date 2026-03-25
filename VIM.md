# Vim Motion Checklist for `frame`

`frame` is a read-only review IDE, not a text editor.

This checklist tracks the Vim-style motions and navigation behaviors that
should exist in `frame` because they improve code review speed and orientation.
It intentionally excludes editing operators such as `d`, `c`, `y`, `p`, and
insert/replace flows that do not fit the product.

Legend:

- `[x]` implemented in `frame`
- `[ ]` not implemented yet, but desirable

## Core Buffer Movement

- [x] `j` / `k`: move down and up by line
- [x] `gg`: jump to the top of the current view buffer
- [x] `G`: jump to the bottom of the current view buffer
- [x] `Ctrl-d`: half-page down
- [x] `Ctrl-u`: half-page up
- [ ] `Ctrl-f` / `Ctrl-b`: full-page down and up
- [x] `H`: jump to the top visible line
- [ ] `M`: jump to the middle visible line
- [x] `L`: jump to the bottom visible line
- [x] `zz`: center the cursor line in the viewport
- [x] `zt`: place the cursor line at the top of the viewport
- [ ] `zb`: place the cursor line at the bottom of the viewport

## Count Prefixes

Numeric prefixes should be treated as part of the motion system, not as a later
editor feature.

- [x] `{count}j` / `{count}k`: move by count lines, for example `3j`
- [x] `{count}gg`: jump to absolute line number in the current buffer, for example `42gg`
- [x] `{count}G`: jump to absolute line number or buffer end when no count is given
- [x] `{count}Ctrl-d` / `{count}Ctrl-u`: scale page motion by count
- [x] `{count}]c` / `{count}[c`: jump by count review changes
- [x] `{count}]f` / `{count}[f`: jump by count changed files
- [ ] `{count}` support should apply to any future motion where it makes sense, including `w`, `b`, `n`, and `N`

Examples:

- `3j`: move down three lines
- `5k`: move up five lines
- `2]c`: jump forward two changes
- `3h`: move left three semantic chunks

## Horizontal and In-Line Motion

`frame` now uses a chunk-first horizontal cursor in code view. Movement is
semantic rather than character-column-based, which fits a read-only review IDE
better and creates a cleaner path for future LSP operations.

- [x] `h` / `l`: move left and right across semantic chunks, wrapping to the previous or next line when needed
- [x] `0`: jump to the first chunk on the line
- [x] `^`: jump to the first non-blank chunk on the line
- [x] `$`: jump to the last chunk on the line
- [ ] `w` / `b`: jump to next and previous word start
- [ ] `e` / `ge`: jump to next and previous word end

## Search and Symbol-Oriented Motion

- [ ] `/`: forward search
- [ ] `?`: backward search
- [ ] `n` / `N`: repeat search forward and backward
- [ ] `*` / `#`: search for word under cursor forward and backward
- [ ] `%`: jump between matching delimiters
- [ ] `gd`: go to definition
  Reserved for future symbol navigation. `gd` still exists today as a compatibility alias for raw diff toggle, but `Tab` is the canonical binding.

## Jump List, Marks, and Reorientation

- [ ] `Ctrl-o` / `Ctrl-i`: jump backward and forward through location history
- [ ] `''` / ````: jump back to previous line or exact cursor location
- [ ] `m{char}`: set a mark
- [ ] `'{char}` / ``{char}``: jump to a mark by line or exact location

## Visual Selection

- [x] `v`: visual selection
  Current behavior: chunk-aware selection in code view, used for review comments rather than editing.
- [ ] `V`: explicit linewise visual mode
- [ ] `Ctrl-v`: blockwise visual mode
- [ ] `o`: swap visual selection anchor and active edge

## Line Numbering

Line numbers are part of the navigation model because they affect how users
orient themselves and how useful motions like `5j` or `42gg` feel.

- [ ] hybrid line numbers in code view
  Recommended behavior: absolute number on the cursor line, relative numbers on surrounding lines.
- [x] absolute old/new line numbers in raw diff view
- [ ] `:set number`-style absolute numbering mode, if configuration is added later
- [ ] `:set relativenumber`-style toggle, if configuration is added later

Recommended policy:

- code view should default to hybrid relative numbering
- raw diff view should keep absolute diff line numbers because they are patch data
- virtual deleted lines should preserve their old absolute line numbers, not relative synthetic ones

## Review-Specific Motion

These are not standard Vim motions, but they are core to `frame` and should be
tracked alongside Vim behavior because they define the review experience.

- [x] `]c` / `[c`: next and previous change
- [x] `]f` / `[f`: next and previous changed file
- [x] `]h` / `[h`: next and previous raw diff hunk in raw diff mode
- [x] `Tab`: toggle code view and raw diff view
  Canonical binding. `gd` is a temporary compatibility alias until `go to definition` exists.
- [ ] `A`: send queued comments to AI and reload the review loop

## Command and Review Affordances

These are not motions, but they are part of the keyboard-first control surface.

- [x] `:`: command prompt
- [x] `i`: start an inline AI comment on the current chunk or visual selection
- [x] `e`: toggle the file explorer
- [x] `q`: quit
- [x] `Ctrl-c`: hard quit

## Intentionally Out of Scope

These should not be treated as required for `frame` unless the product stops
being read-only.

- [ ] insert mode for code editing
- [ ] replace mode for code editing
- [ ] delete, change, yank, paste operators
- [ ] text-object editing commands such as `ci(` or `da{`
- [ ] macros, registers, and replay for code editing

## Notes

- Numeric count prefixes are already part of the motion model and should be
  preserved as new motions are added.
- The second group to add should be `zt`, `zb`, `H`, `M`, and `L` because they
  improve reorientation without requiring a full horizontal cursor model.
- Hybrid relative line numbers are a better default for code view than absolute
  line numbers because `frame` is keyboard-first and read-only.
- New horizontal motions should compose with the chunk cursor rather than
  reintroducing raw character-column navigation as the primary model.
