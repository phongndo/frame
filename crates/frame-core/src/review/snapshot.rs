use std::path::PathBuf;

use crate::{
    PatchFile,
    review::overlay::derive_review_data,
    syntax::{ChunkedFile, HighlightedFile, LanguageId, chunk_buffer, highlight_buffer},
};

use super::{ChangeAnchor, CodeBuffer, DeletedLine, OverlaySpan};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BufferSource {
    PostImage,
    PreImage,
    Placeholder,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReviewFileInput {
    pub patch: PatchFile,
    pub buffer: CodeBuffer,
    pub source: BufferSource,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReviewSnapshot {
    pub repo_root: PathBuf,
    pub files: Vec<ReviewFile>,
}

impl ReviewSnapshot {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.files.is_empty()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReviewFile {
    pub patch: PatchFile,
    pub buffer: CodeBuffer,
    pub source: BufferSource,
    pub language: Option<LanguageId>,
    pub highlights: Option<HighlightedFile>,
    pub chunks: ChunkedFile,
    pub overlays: Vec<OverlaySpan>,
    pub deleted_lines: Vec<DeletedLine>,
    pub anchors: Vec<ChangeAnchor>,
}

impl ReviewFile {
    #[must_use]
    pub fn new(input: ReviewFileInput) -> Self {
        let derived = derive_review_data(&input.patch, &input.buffer, input.source);
        let language = (!matches!(input.source, BufferSource::Placeholder))
            .then(|| LanguageId::detect(input.patch.display_path()))
            .flatten();
        let highlights = language.and_then(|language| highlight_buffer(language, &input.buffer));
        let chunks = if matches!(input.source, BufferSource::Placeholder) {
            ChunkedFile::empty(input.buffer.line_count())
        } else {
            chunk_buffer(language, &input.buffer)
        };

        Self {
            patch: input.patch,
            buffer: input.buffer,
            source: input.source,
            language,
            highlights,
            chunks,
            overlays: derived.overlays,
            deleted_lines: derived.deleted_lines,
            anchors: derived.anchors,
        }
    }

    #[must_use]
    pub fn display_path(&self) -> &str {
        self.patch.display_path()
    }

    #[must_use]
    pub fn line_change(&self, line_index: usize) -> Option<super::ChangeKind> {
        self.overlays
            .iter()
            .find(|overlay| overlay.start_line <= line_index && line_index <= overlay.end_line)
            .map(|overlay| overlay.kind)
    }

    #[must_use]
    pub fn highlighted_line(&self, line_index: usize) -> Option<&crate::HighlightedLine> {
        self.highlights.as_ref()?.line(line_index)
    }

    #[must_use]
    pub fn chunk(&self, line_index: usize, chunk_index: usize) -> Option<&crate::NavigableChunk> {
        self.chunks.chunk(line_index, chunk_index)
    }
}

#[cfg(test)]
mod tests {
    use crate::{
        FileChangeKind, LineKind, PatchFile, PatchHunk, PatchLine,
        review::{BufferSource, ChangeKind, CodeBuffer, ReviewFile, ReviewFileInput},
    };

    #[test]
    fn derives_modified_overlays_and_virtual_deleted_lines() {
        let file = ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: Some("src/main.rs".to_owned()),
                new_path: Some("src/main.rs".to_owned()),
                change: FileChangeKind::Modified,
                hunks: vec![PatchHunk {
                    header: "@@ -1,3 +1,3 @@".to_owned(),
                    old_start: 1,
                    old_len: 3,
                    new_start: 1,
                    new_len: 3,
                    lines: vec![
                        PatchLine {
                            kind: LineKind::Context,
                            old_lineno: Some(1),
                            new_lineno: Some(1),
                            text: "fn main() {".to_owned(),
                        },
                        PatchLine {
                            kind: LineKind::Removed,
                            old_lineno: Some(2),
                            new_lineno: None,
                            text: "    old();".to_owned(),
                        },
                        PatchLine {
                            kind: LineKind::Added,
                            old_lineno: None,
                            new_lineno: Some(2),
                            text: "    new();".to_owned(),
                        },
                        PatchLine {
                            kind: LineKind::Context,
                            old_lineno: Some(3),
                            new_lineno: Some(3),
                            text: "}".to_owned(),
                        },
                    ],
                }],
                has_binary_or_unrenderable_change: false,
            },
            buffer: CodeBuffer::from_text("fn main() {\n    new();\n}\n"),
            source: BufferSource::PostImage,
        });

        assert_eq!(file.overlays.len(), 1);
        assert_eq!(file.overlays[0].kind, ChangeKind::Modified);
        assert_eq!(file.overlays[0].start_line, 1);
        assert_eq!(file.overlays[0].end_line, 1);
        assert_eq!(file.language, Some(crate::LanguageId::Rust));
        assert!(file.highlights.is_some());
        assert_eq!(file.deleted_lines.len(), 1);
        assert_eq!(file.deleted_lines[0].anchor_line, 1);
        assert_eq!(file.anchors[0].buffer_line, 1);
    }

    #[test]
    fn derives_deleted_file_overlays_on_preimage_buffer() {
        let file = ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: Some("old.txt".to_owned()),
                new_path: None,
                change: FileChangeKind::Deleted,
                hunks: vec![PatchHunk {
                    header: "@@ -1,2 +0,0 @@".to_owned(),
                    old_start: 1,
                    old_len: 2,
                    new_start: 0,
                    new_len: 0,
                    lines: vec![
                        PatchLine {
                            kind: LineKind::Removed,
                            old_lineno: Some(1),
                            new_lineno: None,
                            text: "left".to_owned(),
                        },
                        PatchLine {
                            kind: LineKind::Removed,
                            old_lineno: Some(2),
                            new_lineno: None,
                            text: "right".to_owned(),
                        },
                    ],
                }],
                has_binary_or_unrenderable_change: false,
            },
            buffer: CodeBuffer::from_text("left\nright\n"),
            source: BufferSource::PreImage,
        });

        assert_eq!(file.overlays.len(), 1);
        assert_eq!(file.overlays[0].kind, ChangeKind::Deleted);
        assert_eq!(file.language, None);
        assert!(file.deleted_lines.is_empty());
        assert_eq!(file.anchors[0].buffer_line, 0);
    }
}
