#![doc = r"
`frame-core` hosts the UI-agnostic patch parsing and review-domain logic for
Frame.

The crate keeps `git diff` as the source of truth for change metadata, then
derives editor-like buffers and change overlays for the TUI to render.
"]

pub mod patch;
pub mod review;
pub mod syntax;

pub use patch::{
    FileChangeKind, LineKind, PatchFile, PatchHunk, PatchLine, PatchParseError, PatchSet,
    parse_patch,
};
pub use review::{
    BufferSource, ChangeAnchor, ChangeKind, CodeBuffer, DeletedLine, OverlaySpan, ReviewFile,
    ReviewFileInput, ReviewSnapshot,
};
pub use syntax::{
    BufferPoint, BufferSpan, ChunkKind, ChunkRole, ChunkedFile, ChunkedLine, NavigableChunk,
};
pub use syntax::{HighlightSpan, HighlightStyleKey, HighlightedFile, HighlightedLine, LanguageId};
