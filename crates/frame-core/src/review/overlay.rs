use crate::{
    FileChangeKind, LineKind, PatchFile, PatchLine,
    review::{BufferSource, CodeBuffer},
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum ChangeKind {
    Added,
    Modified,
    Deleted,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverlaySpan {
    pub start_line: usize,
    pub end_line: usize,
    pub kind: ChangeKind,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeletedLine {
    pub anchor_line: usize,
    pub old_lineno: Option<usize>,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChangeAnchor {
    pub buffer_line: usize,
    pub kind: ChangeKind,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub(crate) struct DerivedReviewData {
    pub overlays: Vec<OverlaySpan>,
    pub deleted_lines: Vec<DeletedLine>,
    pub anchors: Vec<ChangeAnchor>,
}

#[derive(Debug, Default)]
struct ChangeBlock<'a> {
    lines: Vec<&'a PatchLine>,
}

impl<'a> ChangeBlock<'a> {
    fn push(&mut self, line: &'a PatchLine) {
        self.lines.push(line);
    }

    fn take(&mut self) -> Vec<&'a PatchLine> {
        std::mem::take(&mut self.lines)
    }
}

pub(crate) fn derive_review_data(
    patch: &PatchFile,
    buffer: &CodeBuffer,
    source: BufferSource,
) -> DerivedReviewData {
    let mut derived = DerivedReviewData::default();

    if matches!(source, BufferSource::Placeholder) {
        derived.anchors.push(ChangeAnchor {
            buffer_line: 0,
            kind: change_kind_for_file(patch.change),
        });
        return derived;
    }

    let mut last_visible_new_lineno = None;

    for hunk in &patch.hunks {
        let mut block = ChangeBlock::default();

        for line in &hunk.lines {
            if line.kind == LineKind::Context {
                let block_lines = block.take();
                flush_block(
                    patch,
                    &mut derived,
                    buffer,
                    source,
                    &block_lines,
                    last_visible_new_lineno,
                    line.new_lineno,
                );
                if let Some(new_lineno) = line.new_lineno {
                    last_visible_new_lineno = Some(new_lineno);
                }
            } else {
                block.push(line);
                if let Some(new_lineno) = line.new_lineno {
                    last_visible_new_lineno = Some(new_lineno);
                }
            }
        }

        let block_lines = block.take();
        flush_block(
            patch,
            &mut derived,
            buffer,
            source,
            &block_lines,
            last_visible_new_lineno,
            None,
        );
    }

    if derived.anchors.is_empty() && !patch.hunks.is_empty() {
        derived.anchors.push(ChangeAnchor {
            buffer_line: 0,
            kind: change_kind_for_file(patch.change),
        });
    }

    derived
        .deleted_lines
        .sort_by(|left, right| left.anchor_line.cmp(&right.anchor_line));
    derived.anchors.sort_by(|left, right| {
        left.buffer_line
            .cmp(&right.buffer_line)
            .then(left.kind.cmp(&right.kind))
    });
    derived
        .anchors
        .dedup_by(|left, right| left.buffer_line == right.buffer_line && left.kind == right.kind);

    derived
}

fn flush_block(
    patch: &PatchFile,
    derived: &mut DerivedReviewData,
    buffer: &CodeBuffer,
    source: BufferSource,
    block: &[&PatchLine],
    previous_visible_new_lineno: Option<usize>,
    next_visible_new_lineno: Option<usize>,
) {
    if block.is_empty() {
        return;
    }

    let has_added = block.iter().any(|line| line.kind == LineKind::Added);
    let has_removed = block.iter().any(|line| line.kind == LineKind::Removed);

    match source {
        BufferSource::PostImage => {
            if has_added {
                let start_line = first_new_line(block, buffer);
                let end_line = last_new_line(block, buffer);
                let kind = if has_removed {
                    ChangeKind::Modified
                } else {
                    ChangeKind::Added
                };
                derived.overlays.push(OverlaySpan {
                    start_line,
                    end_line,
                    kind,
                });
                derived.anchors.push(ChangeAnchor {
                    buffer_line: start_line,
                    kind,
                });

                if has_removed {
                    push_deleted_virtuals(derived, block, buffer, start_line);
                }
            } else if has_removed {
                let anchor_line = deletion_anchor_line(
                    buffer,
                    previous_visible_new_lineno,
                    next_visible_new_lineno,
                );
                push_deleted_virtuals(derived, block, buffer, anchor_line);
                derived.anchors.push(ChangeAnchor {
                    buffer_line: anchor_line.min(buffer.line_count().saturating_sub(1)),
                    kind: ChangeKind::Deleted,
                });
            }
        }
        BufferSource::PreImage => {
            if has_removed {
                let start_line = first_old_line(block, buffer);
                let end_line = last_old_line(block, buffer);
                derived.overlays.push(OverlaySpan {
                    start_line,
                    end_line,
                    kind: ChangeKind::Deleted,
                });
                derived.anchors.push(ChangeAnchor {
                    buffer_line: start_line,
                    kind: ChangeKind::Deleted,
                });
            } else if has_added {
                derived.anchors.push(ChangeAnchor {
                    buffer_line: 0,
                    kind: change_kind_for_file(patch.change),
                });
            }
        }
        BufferSource::Placeholder => {}
    }
}

fn push_deleted_virtuals(
    derived: &mut DerivedReviewData,
    block: &[&PatchLine],
    buffer: &CodeBuffer,
    anchor_line: usize,
) {
    let anchor_line = anchor_line.min(buffer.line_count());

    for line in block.iter().filter(|line| line.kind == LineKind::Removed) {
        derived.deleted_lines.push(DeletedLine {
            anchor_line,
            old_lineno: line.old_lineno,
            text: line.text.clone(),
        });
    }
}

fn deletion_anchor_line(
    buffer: &CodeBuffer,
    previous_visible_new_lineno: Option<usize>,
    next_visible_new_lineno: Option<usize>,
) -> usize {
    if let Some(next_lineno) = next_visible_new_lineno {
        return next_lineno.saturating_sub(1).min(buffer.line_count());
    }

    if let Some(previous_lineno) = previous_visible_new_lineno {
        return previous_lineno.min(buffer.line_count());
    }

    0
}

fn first_new_line(block: &[&PatchLine], buffer: &CodeBuffer) -> usize {
    block
        .iter()
        .find_map(|line| line.new_lineno)
        .map_or(0, |lineno| normalize_line_index(lineno, buffer))
}

fn last_new_line(block: &[&PatchLine], buffer: &CodeBuffer) -> usize {
    block
        .iter()
        .rev()
        .find_map(|line| line.new_lineno)
        .map_or_else(
            || buffer.line_count().saturating_sub(1),
            |lineno| normalize_line_index(lineno, buffer),
        )
}

fn first_old_line(block: &[&PatchLine], buffer: &CodeBuffer) -> usize {
    block
        .iter()
        .find_map(|line| line.old_lineno)
        .map_or(0, |lineno| normalize_line_index(lineno, buffer))
}

fn last_old_line(block: &[&PatchLine], buffer: &CodeBuffer) -> usize {
    block
        .iter()
        .rev()
        .find_map(|line| line.old_lineno)
        .map_or_else(
            || buffer.line_count().saturating_sub(1),
            |lineno| normalize_line_index(lineno, buffer),
        )
}

fn normalize_line_index(lineno: usize, buffer: &CodeBuffer) -> usize {
    lineno
        .saturating_sub(1)
        .min(buffer.line_count().saturating_sub(1))
}

fn change_kind_for_file(change: FileChangeKind) -> ChangeKind {
    match change {
        FileChangeKind::Added => ChangeKind::Added,
        FileChangeKind::Deleted => ChangeKind::Deleted,
        FileChangeKind::Copied | FileChangeKind::Modified | FileChangeKind::Renamed => {
            ChangeKind::Modified
        }
    }
}
