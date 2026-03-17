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
- [ ] `H` / `M` / `L`: jump to top, middle, and bottom visible line
- [ ] `zz`: center the cursor line in the viewport
- [ ] `zt` / `zb`: place the cursor line at the top or bottom of the viewport

## Count Prefixes

Numeric prefixes should be treated as part of the motion system, not as a later
editor feature.

- [ ] `{count}j` / `{count}k`: move by count lines, for example `3j`
- [ ] `{count}gg`: jump to absolute line number in the current buffer, for example `42gg`
- [ ] `{count}G`: jump to absolute line number or buffer end when no count is given
- [ ] `{count}Ctrl-d` / `{count}Ctrl-u`: scale page motion by count
- [ ] `{count}]c` / `{count}[c`: jump by count review changes
- [ ] `{count}]f` / `{count}[f`: jump by count changed files
- [ ] `{count}` support should apply to any future motion where it makes sense, including `h`, `l`, `w`, `b`, `n`, and `N`

Examples:

- `3j`: move down three lines
- `5k`: move up five lines
- `2]c`: jump forward two changes
- `3h`: once horizontal motion exists, move left three columns

## Horizontal and In-Line Motion

These depend on adding a real column-aware cursor model. They are worth having,
but should come after viewport and review navigation are solid.

- [ ] `h` / `l`: move left and right within a line
- [ ] `0`: jump to the start of the line
- [ ] `^`: jump to the first non-blank character on the line
- [ ] `$`: jump to the end of the line
- [ ] `w` / `b`: jump to next and previous word start
- [ ] `e` / `ge`: jump to next and previous word end

## Search and Symbol-Oriented Motion

- [ ] `/`: forward search
- [ ] `?`: backward search
- [ ] `n` / `N`: repeat search forward and backward
- [ ] `*` / `#`: search for word under cursor forward and backward
- [ ] `%`: jump between matching delimiters
- [ ] `gd`: go to definition
  Current conflict: `gd` is used today for raw diff toggle and should be freed if LSP-style navigation is added.

## Jump List, Marks, and Reorientation

- [ ] `Ctrl-o` / `Ctrl-i`: jump backward and forward through location history
- [ ] `''` / ````: jump back to previous line or exact cursor location
- [ ] `m{char}`: set a mark
- [ ] `'{char}` / ``{char}``: jump to a mark by line or exact location

## Visual Selection

- [x] `v`: visual selection
  Current behavior: line-range selection only, used for review comments rather than editing.
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
- [ ] `A`: send queued comments to AI and reload the review loop

## Command and Review Affordances

These are not motions, but they are part of the keyboard-first control surface.

- [x] `:`: command prompt
- [x] `i`: start an inline AI comment on the current line or visual selection
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

- The first missing motion to add is `zz`. It is simple, high value, and fits
  the existing line-oriented cursor model.
- Numeric count prefixes should be added at the same time or immediately after
  `zz`, because they define how motions scale in a Vim-like interface.
- The second group to add should be `zt`, `zb`, `H`, `M`, and `L` because they
  improve reorientation without requiring a full horizontal cursor model.
- Hybrid relative line numbers are a better default for code view than absolute
  line numbers because `frame` is keyboard-first and read-only.
- Do not implement horizontal word motions until the application has a real
  column cursor and consistent byte/character position handling.
