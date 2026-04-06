# Git Ship-Lane Checklist for `frame`

`frame` is a review-first terminal IDE, not a general-purpose Git console.

This checklist tracks the Git operations that belong inside `frame` because
they tighten the review-to-ship loop after AI or manual code generation. It
intentionally excludes broad repository administration features that would turn
the product into a full LazyGit replacement.

Legend:

- `[x]` implemented in `frame`
- `[ ]` not implemented yet, but still in scope or on the roadmap

## Current Ship Lane

- [x] `Ctrl-g` or `:git`: open the floating Git panel
- [x] branch name, upstream, ahead/behind, and staged/unstaged summaries
- [x] staged and unstaged file trees, expandable by file and hunk
- [x] sidebar staged/unstaged badges per changed file
- [x] `s`: toggle stage for the selected file, hunk, or line in the Git panel
- [x] `s`: toggle stage for the current reviewed change from code view or raw diff
- [x] `C`: open the commit dialog from the review surface
- [x] new commit creation from staged changes
- [x] amend `HEAD` from the Git panel
- [x] `P`: push the current branch
- [x] `F`: force-with-lease push for amended or rewritten branch tips
- [x] `R`: create or refresh the current branch pull request via `gh`
- [x] render pull-request check summaries inside the Git panel

## Partial Line Staging

- [x] stage or unstage directly selectable added and removed diff lines
- [ ] arbitrary mixed-hunk line splitting with context-aware patch synthesis

## Short-Term Next

- [ ] current-branch sync from inside `frame`
- [ ] explicit PR open action in addition to create/refresh
- [ ] clearer push/pull error handling for auth, missing upstreams, and non-fast-forward conflicts
- [ ] direct surface affordances for amend from the main review view

## Long-Term Roadmap

- [ ] branch create and branch switch
- [ ] worktree management
- [ ] stash create, apply, and drop
- [ ] fetch and pull beyond current-branch sync
- [ ] rebase flows
- [ ] cherry-pick flows
- [ ] reset and revert flows
- [ ] bisect flows
- [ ] full pull-request console, including review threads and approvals

## Intentionally Out of Scope for This Layer

- [ ] full repo dashboard that competes with the review view
- [ ] history rewriting UI beyond the narrow amend plus force-with-lease flow
- [ ] editor-like file editing inside the ship lane

## Notes

- The current implementation is intentionally current-branch-only. `frame`
  should help review and ship the branch you are already on.
- First push prefers `branch.<name>.remote` when Git has one configured for the
  current branch and otherwise falls back to `origin`.
- GitHub integration is `gh`-backed today. When `gh` is unavailable,
  review features still work, but PR actions and check summaries do not.
- The review surface remains primary. The Git panel is a secondary overlay,
  not the main screen.
