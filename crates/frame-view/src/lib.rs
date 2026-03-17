use std::{collections::BTreeMap, fmt::Write as _, io, time::Duration};

use frame_core::{
    BufferSource, BufferSpan, ChangeKind, HighlightStyleKey, HighlightedLine, NavigableChunk,
    ReviewFile, ReviewSnapshot,
};
use ratatui::{
    Frame, Terminal,
    backend::CrosstermBackend,
    crossterm::{
        event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers},
        execute,
        terminal::{EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode},
    },
    layout::{Constraint, Layout},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{List, ListItem, ListState, Paragraph},
};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ViewError {
    #[error("failed to interact with the terminal: {0}")]
    Io(#[from] io::Error),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ViewMode {
    Code,
    RawDiff,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum InputMode {
    Normal,
    Command(String),
    Comment(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MotionMode {
    Normal,
    Visual,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PendingSequence {
    None,
    G,
    OpenBracket,
    CloseBracket,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CodeRowKind {
    Buffer,
    VirtualDeleted,
    Banner,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CodeRenderRow {
    kind: CodeRowKind,
    buffer_line: Option<usize>,
    lineno: Option<usize>,
    text: String,
    change: Option<ChangeKind>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RawRowKind {
    HunkHeader,
    Added,
    Removed,
    Context,
    Placeholder,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct RawRenderRow {
    kind: RawRowKind,
    old_lineno: Option<usize>,
    new_lineno: Option<usize>,
    text: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum CommentTarget {
    ChunkSpan(BufferSpan),
    LineRange { start_line: usize, end_line: usize },
}

impl CommentTarget {
    fn normalized(&self) -> Self {
        match self {
            Self::ChunkSpan(span) => Self::ChunkSpan(span.normalized()),
            Self::LineRange {
                start_line,
                end_line,
            } => Self::LineRange {
                start_line: (*start_line).min(*end_line),
                end_line: (*start_line).max(*end_line),
            },
        }
    }

    fn intersects_line(&self, line_index: usize) -> bool {
        match self.normalized() {
            Self::ChunkSpan(span) => span.intersects_line(line_index),
            Self::LineRange {
                start_line,
                end_line,
            } => start_line <= line_index && line_index <= end_line,
        }
    }

    fn end_line(&self) -> usize {
        match self.normalized() {
            Self::ChunkSpan(span) => span.end.line,
            Self::LineRange { end_line, .. } => end_line,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CursorAnchor {
    line: usize,
    chunk_index: Option<usize>,
    preferred_display_col: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ReviewComment {
    file_path: String,
    target: CommentTarget,
    text: String,
}

#[derive(Debug)]
struct App {
    snapshot: ReviewSnapshot,
    active_file_index: usize,
    file_explorer_open: bool,
    code_cursor_line: usize,
    code_cursor_chunk: Option<usize>,
    code_preferred_display_col: usize,
    code_viewport_top: usize,
    raw_cursor_line: usize,
    raw_viewport_top: usize,
    viewport_height: usize,
    viewport_width: usize,
    pending_sequence: PendingSequence,
    pending_count: Option<usize>,
    view_mode: ViewMode,
    motion_mode: MotionMode,
    visual_anchor: Option<CursorAnchor>,
    input_mode: InputMode,
    comments: Vec<ReviewComment>,
    status_message: String,
}

impl App {
    fn new(snapshot: ReviewSnapshot) -> Self {
        let mut app = Self {
            snapshot,
            active_file_index: 0,
            file_explorer_open: true,
            code_cursor_line: 0,
            code_cursor_chunk: None,
            code_preferred_display_col: 0,
            code_viewport_top: 0,
            raw_cursor_line: 0,
            raw_viewport_top: 0,
            viewport_height: 1,
            viewport_width: 1,
            pending_sequence: PendingSequence::None,
            pending_count: None,
            view_mode: ViewMode::Code,
            motion_mode: MotionMode::Normal,
            visual_anchor: None,
            input_mode: InputMode::Normal,
            comments: Vec::new(),
            status_message: "Press : for commands, i to queue a comment for AI.".to_owned(),
        };
        app.reset_active_file_positions();
        app
    }

    fn active_file(&self) -> Option<&ReviewFile> {
        self.snapshot.files.get(self.active_file_index)
    }

    fn set_status(&mut self, message: &str) {
        message.clone_into(&mut self.status_message);
    }

    fn clear_visual_mode(&mut self) {
        self.motion_mode = MotionMode::Normal;
        self.visual_anchor = None;
    }

    fn cursor_anchor(&self) -> CursorAnchor {
        CursorAnchor {
            line: self.code_cursor_line,
            chunk_index: self.code_cursor_chunk,
            preferred_display_col: self.code_preferred_display_col,
        }
    }

    fn selection_target(&self, file: &ReviewFile) -> Option<CommentTarget> {
        if self.view_mode != ViewMode::Code || self.motion_mode != MotionMode::Visual {
            return None;
        }

        let anchor = self.visual_anchor?;
        let cursor_target = self.current_comment_target(file);
        let anchor_target = Self::comment_target_for_anchor(file, anchor);

        match (anchor_target, cursor_target) {
            (Some(CommentTarget::ChunkSpan(left)), Some(CommentTarget::ChunkSpan(right))) => {
                Some(CommentTarget::ChunkSpan(BufferSpan {
                    start: left.start,
                    end: right.end,
                }))
            }
            _ => Some(CommentTarget::LineRange {
                start_line: anchor.line.min(self.code_cursor_line),
                end_line: anchor.line.max(self.code_cursor_line),
            }),
        }
    }

    fn line_in_selection(&self, file: &ReviewFile, line_index: usize) -> bool {
        self.selection_target(file)
            .is_some_and(|target| target.intersects_line(line_index))
    }

    fn comment_draft(&self) -> Option<&str> {
        match &self.input_mode {
            InputMode::Comment(buffer) => Some(buffer.as_str()),
            InputMode::Normal | InputMode::Command(_) => None,
        }
    }

    fn comment_box_anchor_line(&self, file: &ReviewFile) -> Option<usize> {
        self.comment_draft().map(|_| {
            self.selection_target(file)
                .unwrap_or_else(|| {
                    self.current_comment_target(file)
                        .unwrap_or(CommentTarget::LineRange {
                            start_line: self.code_cursor_line,
                            end_line: self.code_cursor_line,
                        })
                })
                .end_line()
                .min(file.buffer.line_count().saturating_sub(1))
        })
    }

    fn mode_label(&self) -> String {
        match self.motion_mode {
            MotionMode::Normal => "NORMAL".to_owned(),
            MotionMode::Visual => "VISUAL".to_owned(),
        }
    }

    fn push_count_digit(&mut self, digit: char) {
        let value = digit
            .to_digit(10)
            .expect("only decimal digits should be passed");
        let next = self
            .pending_count
            .unwrap_or(0)
            .saturating_mul(10)
            .saturating_add(value as usize);
        self.pending_count = Some(next);
    }

    fn take_count(&mut self) -> usize {
        self.pending_count.take().unwrap_or(1)
    }

    fn clear_prefixes(&mut self) {
        self.pending_sequence = PendingSequence::None;
        self.pending_count = None;
    }

    fn handle_key(&mut self, key: KeyEvent) -> bool {
        let should_process = matches!(key.kind, KeyEventKind::Press | KeyEventKind::Repeat);
        if !should_process {
            return false;
        }

        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            return true;
        }

        if !matches!(self.input_mode, InputMode::Normal) {
            return self.handle_input_key(key);
        }

        if key.modifiers.contains(KeyModifiers::CONTROL) {
            let count = self.take_count();
            self.pending_sequence = PendingSequence::None;

            match key.code {
                KeyCode::Char('d') => self.move_half_page_down(count),
                KeyCode::Char('u') => self.move_half_page_up(count),
                _ => {}
            }

            return false;
        }

        self.handle_normal_key(key)
    }

    fn handle_normal_key(&mut self, key: KeyEvent) -> bool {
        match key.code {
            KeyCode::Char('q') => return true,
            KeyCode::Char(ch) if ch.is_ascii_digit() => {
                if ch == '0' && self.pending_count.is_none() {
                    self.pending_sequence = PendingSequence::None;
                    self.move_to_line_start();
                } else {
                    self.push_count_digit(ch);
                }
            }
            KeyCode::Char(':') => {
                self.clear_prefixes();
                self.input_mode = InputMode::Command(String::new());
            }
            KeyCode::Esc => {
                self.clear_prefixes();
                if self.motion_mode == MotionMode::Visual {
                    self.clear_visual_mode();
                    self.set_status("Visual mode canceled.");
                }
            }
            KeyCode::Char('i') => {
                self.clear_prefixes();
                if self.active_file().is_some() {
                    if self.view_mode == ViewMode::RawDiff {
                        self.view_mode = ViewMode::Code;
                        self.set_status("Switched to code view for commenting.");
                    }
                    self.input_mode = InputMode::Comment(String::new());
                }
            }
            KeyCode::Char('v') => {
                self.pending_sequence = PendingSequence::None;
                self.toggle_visual_mode();
            }
            KeyCode::Char('e') => {
                self.clear_prefixes();
                self.toggle_file_explorer();
            }
            KeyCode::Tab => {
                self.clear_prefixes();
                self.toggle_mode();
            }
            KeyCode::Char('j') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                self.move_down(count);
            }
            KeyCode::Char('k') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                self.move_up(count);
            }
            KeyCode::Char('l') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                self.move_right_chunk(count);
            }
            KeyCode::Char('G') => {
                let count = self.pending_count.take();
                self.pending_sequence = PendingSequence::None;
                self.move_to_end(count);
            }
            KeyCode::Char('g') => {
                self.handle_g_sequence();
            }
            KeyCode::Char('d') => self.handle_d_sequence(),
            KeyCode::Char(']') => self.pending_sequence = PendingSequence::CloseBracket,
            KeyCode::Char('[') => self.pending_sequence = PendingSequence::OpenBracket,
            KeyCode::Char('c') => self.handle_change_sequence(),
            KeyCode::Char('f') => self.handle_file_sequence(),
            KeyCode::Char('^') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                if count > 1 {
                    self.move_to_line_start();
                }
                self.move_to_first_non_blank_chunk();
            }
            KeyCode::Char('$') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                if count > 1 {
                    self.move_down(count - 1);
                }
                self.move_to_line_end();
            }
            KeyCode::Char('h') => self.handle_hunk_sequence(),
            _ => {
                self.clear_prefixes();
            }
        }

        false
    }

    fn handle_g_sequence(&mut self) {
        if self.pending_sequence == PendingSequence::G {
            let count = self.pending_count.take();
            self.move_to_start(count);
            self.pending_sequence = PendingSequence::None;
        } else {
            self.pending_sequence = PendingSequence::G;
        }
    }

    fn handle_d_sequence(&mut self) {
        if self.pending_sequence == PendingSequence::G {
            self.toggle_mode();
        }
        self.pending_sequence = PendingSequence::None;
    }

    fn handle_change_sequence(&mut self) {
        let count = self.take_count();
        if self.pending_sequence == PendingSequence::CloseBracket {
            self.jump_next_change(count);
        } else if self.pending_sequence == PendingSequence::OpenBracket {
            self.jump_previous_change(count);
        }
        self.pending_sequence = PendingSequence::None;
    }

    fn handle_file_sequence(&mut self) {
        let count = self.take_count();
        if self.pending_sequence == PendingSequence::CloseBracket {
            self.jump_next_file(count);
        } else if self.pending_sequence == PendingSequence::OpenBracket {
            self.jump_previous_file(count);
        }
        self.pending_sequence = PendingSequence::None;
    }

    fn handle_hunk_sequence(&mut self) {
        let count = self.take_count();
        if self.pending_sequence == PendingSequence::CloseBracket {
            self.jump_next_hunk(count);
        } else if self.pending_sequence == PendingSequence::OpenBracket {
            self.jump_previous_hunk(count);
        } else if self.view_mode == ViewMode::Code {
            self.move_left_chunk(count);
        }
        self.pending_sequence = PendingSequence::None;
    }

    fn handle_input_key(&mut self, key: KeyEvent) -> bool {
        match &mut self.input_mode {
            InputMode::Normal => false,
            InputMode::Command(buffer) => match key.code {
                KeyCode::Esc => {
                    self.input_mode = InputMode::Normal;
                    false
                }
                KeyCode::Enter => {
                    let command = buffer.trim().to_owned();
                    self.input_mode = InputMode::Normal;
                    self.execute_command(&command)
                }
                KeyCode::Backspace => {
                    buffer.pop();
                    false
                }
                KeyCode::Char(ch)
                    if key.modifiers.is_empty() || key.modifiers == KeyModifiers::SHIFT =>
                {
                    buffer.push(ch);
                    false
                }
                _ => false,
            },
            InputMode::Comment(buffer) => match key.code {
                KeyCode::Esc => {
                    self.input_mode = InputMode::Normal;
                    self.set_status("Comment canceled.");
                    false
                }
                KeyCode::Enter => {
                    let comment = buffer.trim().to_owned();
                    self.input_mode = InputMode::Normal;
                    self.submit_comment(comment);
                    false
                }
                KeyCode::Backspace => {
                    buffer.pop();
                    false
                }
                KeyCode::Char(ch)
                    if key.modifiers.is_empty() || key.modifiers == KeyModifiers::SHIFT =>
                {
                    buffer.push(ch);
                    false
                }
                _ => false,
            },
        }
    }

    fn execute_command(&mut self, command: &str) -> bool {
        if command.is_empty() {
            self.set_status("Command canceled.");
            return false;
        }

        match command {
            "q" | "quit" => return true,
            "code" => {
                self.view_mode = ViewMode::Code;
                self.set_status("Switched to code view.");
            }
            "diff" => {
                self.view_mode = ViewMode::RawDiff;
                if let Some(file) = self.active_file() {
                    self.raw_cursor_line = raw_row_for_buffer_line(file, self.code_cursor_line);
                }
                self.set_status("Switched to raw diff view.");
            }
            "comments" => {
                self.status_message = if self.comments.is_empty() {
                    "No queued comments for AI.".to_owned()
                } else {
                    format!("{} queued comments for AI.", self.comments.len())
                };
            }
            "help" => {
                self.set_status("Commands: :q, :code, :diff, :comments, :help.");
            }
            _ => {
                self.status_message = format!("Unknown command: {command}");
            }
        }

        false
    }

    fn submit_comment(&mut self, comment: String) {
        if comment.is_empty() {
            self.set_status("Comment canceled.");
            return;
        }

        let Some((file_path, target)) = self.active_file().and_then(|file| {
            Some((
                file.display_path().to_owned(),
                self.selection_target(file)
                    .or_else(|| self.current_comment_target(file))?,
            ))
        }) else {
            self.set_status("No active file for comment.");
            return;
        };

        let display_target = format_comment_target(&file_path, &target);

        self.comments.push(ReviewComment {
            file_path: file_path.clone(),
            target,
            text: comment,
        });
        self.clear_visual_mode();
        self.status_message =
            format!("Queued AI comment on {display_target}. Use :comments to review.");
    }

    fn current_chunk<'a>(&self, file: &'a ReviewFile) -> Option<&'a NavigableChunk> {
        self.code_cursor_chunk
            .and_then(|index| file.chunk(self.code_cursor_line, index))
    }

    fn comment_target_for_anchor(file: &ReviewFile, anchor: CursorAnchor) -> Option<CommentTarget> {
        anchor
            .chunk_index
            .and_then(|index| file.chunk(anchor.line, index))
            .map(|chunk| CommentTarget::ChunkSpan(chunk.span))
            .or(Some(CommentTarget::LineRange {
                start_line: anchor.line,
                end_line: anchor.line,
            }))
    }

    fn current_comment_target(&self, file: &ReviewFile) -> Option<CommentTarget> {
        self.current_chunk(file)
            .map(|chunk| CommentTarget::ChunkSpan(chunk.span))
            .or(Some(CommentTarget::LineRange {
                start_line: self
                    .code_cursor_line
                    .min(file.buffer.line_count().saturating_sub(1)),
                end_line: self
                    .code_cursor_line
                    .min(file.buffer.line_count().saturating_sub(1)),
            }))
    }

    fn set_code_cursor_line_with_preference(&mut self, line: usize) {
        let Some((next_line, next_chunk)) = self.active_file().map(|file| {
            let next_line = line.min(file.buffer.line_count().saturating_sub(1));
            let next_chunk = file
                .chunks
                .nearest_chunk_index(next_line, self.code_preferred_display_col);
            (next_line, next_chunk)
        }) else {
            self.code_cursor_line = 0;
            self.code_cursor_chunk = None;
            self.code_preferred_display_col = 0;
            return;
        };

        self.code_cursor_line = next_line;
        self.code_cursor_chunk = next_chunk;
        self.refresh_preferred_display_col();
    }

    fn set_code_cursor_to_first_chunk(&mut self, line: usize) {
        let Some((next_line, next_chunk)) = self.active_file().map(|file| {
            let next_line = line.min(file.buffer.line_count().saturating_sub(1));
            let next_chunk = file.chunks.first_chunk_index(next_line);
            (next_line, next_chunk)
        }) else {
            self.code_cursor_line = 0;
            self.code_cursor_chunk = None;
            self.code_preferred_display_col = 0;
            return;
        };

        self.code_cursor_line = next_line;
        self.code_cursor_chunk = next_chunk;
        self.refresh_preferred_display_col();
    }

    fn set_code_cursor_to_last_chunk(&mut self, line: usize) {
        let Some((next_line, next_chunk)) = self.active_file().map(|file| {
            let next_line = line.min(file.buffer.line_count().saturating_sub(1));
            let next_chunk = file.chunks.last_chunk_index(next_line);
            (next_line, next_chunk)
        }) else {
            self.code_cursor_line = 0;
            self.code_cursor_chunk = None;
            self.code_preferred_display_col = 0;
            return;
        };

        self.code_cursor_line = next_line;
        self.code_cursor_chunk = next_chunk;
        self.refresh_preferred_display_col();
    }

    fn refresh_preferred_display_col(&mut self) {
        if let Some(display_col) = self
            .active_file()
            .and_then(|file| self.current_chunk(file))
            .map(|chunk| chunk.span.start.display_col)
        {
            self.code_preferred_display_col = display_col;
        }
    }

    fn move_left_chunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::Code {
            return;
        }

        for _ in 0..count {
            let Some((next_line, next_chunk)) = self.active_file().and_then(|file| {
                if let Some(current) = self.code_cursor_chunk {
                    if let Some(previous) = current.checked_sub(1) {
                        return Some((self.code_cursor_line, previous));
                    }
                } else if let Some(last) = file.chunks.last_chunk_index(self.code_cursor_line) {
                    return Some((self.code_cursor_line, last));
                }

                (0..self.code_cursor_line).rev().find_map(|line| {
                    file.chunks
                        .last_chunk_index(line)
                        .map(|chunk| (line, chunk))
                })
            }) else {
                break;
            };
            self.code_cursor_line = next_line;
            self.code_cursor_chunk = Some(next_chunk);
        }
        self.refresh_preferred_display_col();
    }

    fn move_right_chunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::Code {
            return;
        }

        for _ in 0..count {
            let Some((next_line, next_chunk)) = self.active_file().and_then(|file| {
                if let Some(current) = self.code_cursor_chunk {
                    let last = file.chunks.last_chunk_index(self.code_cursor_line)?;
                    if current < last {
                        return Some((self.code_cursor_line, current + 1));
                    }
                } else if let Some(first) = file.chunks.first_chunk_index(self.code_cursor_line) {
                    return Some((self.code_cursor_line, first));
                }

                ((self.code_cursor_line + 1)..file.buffer.line_count()).find_map(|line| {
                    file.chunks
                        .first_chunk_index(line)
                        .map(|chunk| (line, chunk))
                })
            }) else {
                break;
            };
            self.code_cursor_line = next_line;
            self.code_cursor_chunk = Some(next_chunk);
        }
        self.refresh_preferred_display_col();
    }

    fn move_up(&mut self, count: usize) {
        match self.view_mode {
            ViewMode::Code => {
                self.set_code_cursor_line_with_preference(
                    self.code_cursor_line.saturating_sub(count),
                );
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = self.raw_cursor_line.saturating_sub(count);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_down(&mut self, count: usize) {
        match self.view_mode {
            ViewMode::Code => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| file.buffer.line_count().saturating_sub(1));
                self.set_code_cursor_line_with_preference(
                    self.code_cursor_line.saturating_add(count).min(max_index),
                );
            }
            ViewMode::RawDiff => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| raw_rows(file).len().saturating_sub(1));
                self.raw_cursor_line = self.raw_cursor_line.saturating_add(count).min(max_index);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_to_line_start(&mut self) {
        if self.view_mode != ViewMode::Code {
            return;
        }

        self.set_code_cursor_to_first_chunk(self.code_cursor_line);
    }

    fn move_to_first_non_blank_chunk(&mut self) {
        if self.view_mode != ViewMode::Code {
            return;
        }

        self.set_code_cursor_to_first_chunk(self.code_cursor_line);
    }

    fn move_to_line_end(&mut self) {
        if self.view_mode != ViewMode::Code {
            return;
        }

        self.set_code_cursor_to_last_chunk(self.code_cursor_line);
    }

    fn move_to_start(&mut self, count: Option<usize>) {
        match self.view_mode {
            ViewMode::Code => {
                let target = count.unwrap_or(1).saturating_sub(1);
                self.set_code_cursor_to_first_chunk(target);
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = count.unwrap_or(1).saturating_sub(1);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_to_end(&mut self, count: Option<usize>) {
        match self.view_mode {
            ViewMode::Code => {
                let target = count.unwrap_or_else(|| {
                    self.active_file()
                        .map_or(1, |file| file.buffer.line_count())
                });
                self.set_code_cursor_to_last_chunk(target.saturating_sub(1));
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = count.map_or_else(
                    || {
                        self.active_file()
                            .map_or(0, |file| raw_rows(file).len().saturating_sub(1))
                    },
                    |value| value.saturating_sub(1),
                );
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_half_page_down(&mut self, count: usize) {
        let step = self.half_page_step().saturating_mul(count.max(1));

        match self.view_mode {
            ViewMode::Code => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| file.buffer.line_count().saturating_sub(1));
                self.set_code_cursor_line_with_preference(
                    self.code_cursor_line.saturating_add(step).min(max_index),
                );
            }
            ViewMode::RawDiff => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| raw_rows(file).len().saturating_sub(1));
                self.raw_cursor_line = (self.raw_cursor_line + step).min(max_index);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_half_page_up(&mut self, count: usize) {
        let step = self.half_page_step().saturating_mul(count.max(1));

        match self.view_mode {
            ViewMode::Code => {
                self.set_code_cursor_line_with_preference(
                    self.code_cursor_line.saturating_sub(step),
                );
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = self.raw_cursor_line.saturating_sub(step);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn jump_next_change(&mut self, count: usize) {
        match self.view_mode {
            ViewMode::Code => {
                let Some(file) = self.active_file() else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| anchor.buffer_line)
                    .collect::<Vec<_>>();
                if let Some(target) = nth_next_target(&targets, self.code_cursor_line, count) {
                    self.set_code_cursor_to_first_chunk(target);
                }
            }
            ViewMode::RawDiff => {
                let Some(file) = self.active_file() else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| raw_row_for_buffer_line(file, anchor.buffer_line))
                    .collect::<Vec<_>>();
                if let Some(target) = nth_next_target(&targets, self.raw_cursor_line, count) {
                    self.raw_cursor_line = target;
                    self.sync_code_cursor_from_raw();
                }
            }
        }
    }

    fn jump_previous_change(&mut self, count: usize) {
        match self.view_mode {
            ViewMode::Code => {
                let Some(file) = self.active_file() else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| anchor.buffer_line)
                    .collect::<Vec<_>>();
                if let Some(target) = nth_previous_target(&targets, self.code_cursor_line, count) {
                    self.set_code_cursor_to_first_chunk(target);
                }
            }
            ViewMode::RawDiff => {
                let Some(file) = self.active_file() else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| raw_row_for_buffer_line(file, anchor.buffer_line))
                    .collect::<Vec<_>>();
                if let Some(target) = nth_previous_target(&targets, self.raw_cursor_line, count) {
                    self.raw_cursor_line = target;
                    self.sync_code_cursor_from_raw();
                }
            }
        }
    }

    fn jump_next_file(&mut self, count: usize) {
        if self.snapshot.files.is_empty() {
            return;
        }

        let targets = (0..self.snapshot.files.len()).collect::<Vec<_>>();
        if let Some(target) = nth_next_target(&targets, self.active_file_index, count) {
            self.set_active_file(target);
        }
    }

    fn jump_previous_file(&mut self, count: usize) {
        if self.snapshot.files.is_empty() {
            return;
        }

        let targets = (0..self.snapshot.files.len()).collect::<Vec<_>>();
        if let Some(target) = nth_previous_target(&targets, self.active_file_index, count) {
            self.set_active_file(target);
        }
    }

    fn jump_next_hunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(file) = self.active_file() else {
            return;
        };
        let targets = raw_hunk_targets(file);
        if let Some(target) = nth_next_target(&targets, self.raw_cursor_line, count) {
            self.raw_cursor_line = target;
            self.sync_code_cursor_from_raw();
        }
    }

    fn jump_previous_hunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(file) = self.active_file() else {
            return;
        };
        let targets = raw_hunk_targets(file);
        if let Some(target) = nth_previous_target(&targets, self.raw_cursor_line, count) {
            self.raw_cursor_line = target;
            self.sync_code_cursor_from_raw();
        }
    }

    fn toggle_mode(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                if let Some(file) = self.active_file() {
                    self.raw_cursor_line = raw_row_for_buffer_line(file, self.code_cursor_line);
                }
                self.clear_visual_mode();
                self.view_mode = ViewMode::RawDiff;
                self.set_status("Switched to raw diff view.");
            }
            ViewMode::RawDiff => {
                self.view_mode = ViewMode::Code;
                self.set_status("Switched to code view.");
            }
        }
    }

    fn toggle_visual_mode(&mut self) {
        if self.view_mode != ViewMode::Code {
            self.set_status("Visual mode is only available in code view.");
            return;
        }

        match self.motion_mode {
            MotionMode::Normal => {
                self.motion_mode = MotionMode::Visual;
                self.visual_anchor = Some(self.cursor_anchor());
                self.set_status("Visual mode.");
            }
            MotionMode::Visual => {
                self.clear_visual_mode();
                self.set_status("Visual mode canceled.");
            }
        }
    }

    fn toggle_file_explorer(&mut self) {
        self.file_explorer_open = !self.file_explorer_open;
        if self.file_explorer_open {
            self.set_status("Explorer opened.");
        } else {
            self.set_status("Explorer closed.");
        }
    }

    fn half_page_step(&self) -> usize {
        (self.viewport_height.max(1) / 2).max(1)
    }

    fn set_active_file(&mut self, file_index: usize) {
        self.active_file_index = file_index.min(self.snapshot.files.len().saturating_sub(1));
        self.clear_visual_mode();
        self.reset_active_file_positions();
    }

    fn reset_active_file_positions(&mut self) {
        let (code_cursor, code_chunk, code_col, raw_cursor) = if let Some(file) = self.active_file()
        {
            let code_cursor = first_anchor_line(file);
            let code_chunk = file.chunks.first_chunk_index(code_cursor);
            let code_col = code_chunk
                .and_then(|index| file.chunk(code_cursor, index))
                .map_or(0, |chunk| chunk.span.start.display_col);
            let raw_cursor = raw_row_for_buffer_line(file, code_cursor);
            (code_cursor, code_chunk, code_col, raw_cursor)
        } else {
            (0, None, 0, 0)
        };

        self.code_cursor_line = code_cursor;
        self.code_cursor_chunk = code_chunk;
        self.code_preferred_display_col = code_col;
        self.code_viewport_top = 0;
        self.raw_cursor_line = raw_cursor;
        self.raw_viewport_top = 0;
    }

    fn sync_code_cursor_from_raw(&mut self) {
        let new_cursor = self
            .active_file()
            .and_then(|file| buffer_line_for_raw_row(file, self.raw_cursor_line))
            .unwrap_or(self.code_cursor_line);
        self.set_code_cursor_line_with_preference(new_cursor);
    }

    fn sync_viewport(&mut self, height: usize) {
        self.viewport_height = height.max(1);
        self.viewport_width = self.viewport_width.max(1);

        match self.view_mode {
            ViewMode::Code => self.sync_code_viewport(),
            ViewMode::RawDiff => self.sync_raw_viewport(),
        }
    }

    fn set_viewport_size(&mut self, height: usize, width: usize) {
        self.viewport_height = height.max(1);
        self.viewport_width = width.max(1);
    }

    fn sync_code_viewport(&mut self) {
        let Some(file) = self.active_file() else {
            self.code_viewport_top = 0;
            return;
        };
        let rendered = rendered_code_view(self, file, self.viewport_width);
        self.code_viewport_top = sync_viewport_top(
            self.code_viewport_top,
            rendered.cursor_visual_row,
            rendered.lines.len(),
            self.viewport_height,
        );
    }

    fn sync_raw_viewport(&mut self) {
        let Some(file) = self.active_file() else {
            self.raw_viewport_top = 0;
            return;
        };
        let rows = raw_rows(file);
        self.raw_viewport_top = sync_viewport_top(
            self.raw_viewport_top,
            self.raw_cursor_line,
            rows.len(),
            self.viewport_height,
        );
    }

    fn comment_count_for_file(&self, file_path: &str) -> usize {
        self.comments
            .iter()
            .filter(|comment| comment.file_path == file_path)
            .count()
    }

    fn line_has_comment(&self, file_path: &str, line_index: usize) -> bool {
        self.comments.iter().any(|comment| {
            comment.file_path == file_path && comment.target.intersects_line(line_index)
        })
    }

    fn footer_text(&self) -> String {
        let count_prefix = self
            .pending_count
            .map_or(String::new(), |count| format!("{count} "));

        match &self.input_mode {
            InputMode::Normal => format!(
                "{}{} | {} | {} queued | h/l chunks | v visual | e explorer | : commands | i comment | gd/tab toggle | [c/]c change | [f/]f file",
                count_prefix,
                self.mode_label(),
                self.status_message,
                self.comments.len()
            ),
            InputMode::Command(buffer) => format!(":{buffer}"),
            InputMode::Comment(_) => "AI comment | Enter submit | Esc cancel".to_owned(),
        }
    }
}

/// Runs the read-only review IDE until the user quits.
///
/// # Errors
///
/// Returns an error if the terminal cannot be switched into raw/alternate-screen
/// mode, if terminal drawing fails, or if event polling/reading fails.
pub fn run(snapshot: ReviewSnapshot) -> Result<(), ViewError> {
    let mut stdout = io::stdout();
    enable_raw_mode()?;
    execute!(stdout, EnterAlternateScreen)?;

    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;
    let loop_result = run_loop(&mut terminal, App::new(snapshot));
    let restore_result = restore_terminal(&mut terminal);

    loop_result.and(restore_result)
}

fn run_loop(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
    mut app: App,
) -> Result<(), ViewError> {
    loop {
        terminal.draw(|frame| render(frame, &mut app))?;

        if !event::poll(Duration::from_millis(250))? {
            continue;
        }

        if let Event::Key(key) = event::read()?
            && app.handle_key(key)
        {
            return Ok(());
        }
    }
}

fn restore_terminal(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
) -> Result<(), ViewError> {
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}

fn render(frame: &mut Frame<'_>, app: &mut App) {
    let vertical =
        Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).split(frame.area());
    let content_area = if app.file_explorer_open {
        let layout = Layout::horizontal([
            Constraint::Length(32),
            Constraint::Length(1),
            Constraint::Min(10),
        ])
        .split(vertical[0]);

        let mut list_state = ListState::default();
        list_state.select((!app.snapshot.files.is_empty()).then_some(app.active_file_index));

        let file_items = if app.snapshot.files.is_empty() {
            vec![ListItem::new("No changed files")]
        } else {
            app.snapshot
                .files
                .iter()
                .map(|file| {
                    let comment_count = app.comment_count_for_file(file.display_path());
                    let mut label = format!("[{}] {}", file.patch.change, file.display_path());
                    if comment_count > 0 {
                        let _ = write!(label, " !{comment_count}");
                    }
                    ListItem::new(label)
                })
                .collect()
        };
        let file_list = List::new(file_items)
            .highlight_style(
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD),
            )
            .highlight_symbol("> ");
        frame.render_stateful_widget(file_list, layout[0], &mut list_state);

        let separator = vec![
            Line::styled("│", Style::default().fg(Color::DarkGray));
            layout[1].height as usize
        ];
        frame.render_widget(Paragraph::new(separator), layout[1]);
        layout[2]
    } else {
        vertical[0]
    };

    let content_height = content_area.height as usize;
    let content_width = content_area.width as usize;
    app.set_viewport_size(content_height.max(1), content_width.max(1));
    app.sync_viewport(content_height.max(1));

    let content = match app.active_file() {
        Some(file) => match app.view_mode {
            ViewMode::Code => {
                let rendered = rendered_code_view(app, file, content_width.max(1));
                rendered
                    .lines
                    .iter()
                    .skip(app.code_viewport_top)
                    .take(content_height.max(1))
                    .cloned()
                    .collect::<Vec<_>>()
            }
            ViewMode::RawDiff => {
                let rows = raw_rows(file);
                rows.iter()
                    .enumerate()
                    .skip(app.raw_viewport_top)
                    .take(content_height.max(1))
                    .map(|(index, row)| {
                        raw_row_to_text(index == app.raw_cursor_line, row, content_width.max(1))
                    })
                    .collect::<Vec<_>>()
            }
        },
        None => vec![Line::styled(
            "No changes in the current repository.",
            Style::default()
                .fg(Color::DarkGray)
                .add_modifier(Modifier::ITALIC),
        )],
    };

    let content_view = Paragraph::new(content);
    frame.render_widget(content_view, content_area);

    let footer = Paragraph::new(app.footer_text()).style(Style::default().fg(Color::DarkGray));
    frame.render_widget(footer, vertical[1]);
}

fn first_anchor_line(file: &ReviewFile) -> usize {
    file.anchors
        .first()
        .map_or(0, |anchor| anchor.buffer_line)
        .min(file.buffer.line_count().saturating_sub(1))
}

fn sync_viewport_top(
    current_top: usize,
    cursor_row: usize,
    total_rows: usize,
    height: usize,
) -> usize {
    if total_rows <= height {
        return 0;
    }

    let max_top = total_rows.saturating_sub(height);
    if cursor_row < current_top {
        return cursor_row;
    }

    let bottom = current_top.saturating_add(height.saturating_sub(1));
    if cursor_row > bottom {
        return cursor_row
            .saturating_sub(height.saturating_sub(1))
            .min(max_top);
    }

    current_top.min(max_top)
}

fn code_cursor_visual_row(rows: &[CodeRenderRow], cursor_line: usize) -> usize {
    rows.iter()
        .position(|row| row.buffer_line == Some(cursor_line))
        .unwrap_or(0)
}

#[derive(Debug, Clone)]
struct RenderedCodeView {
    lines: Vec<Line<'static>>,
    cursor_visual_row: usize,
}

#[derive(Debug, Clone, Copy)]
struct TextOverlay {
    start_byte: usize,
    end_byte: usize,
    style: Style,
}

fn code_rows(file: &ReviewFile) -> Vec<CodeRenderRow> {
    if matches!(file.source, BufferSource::Placeholder) {
        return vec![CodeRenderRow {
            kind: CodeRowKind::Banner,
            buffer_line: Some(0),
            lineno: None,
            text: file.buffer.line(0).unwrap_or_default().to_owned(),
            change: None,
        }];
    }

    let mut deleted_by_anchor: BTreeMap<usize, Vec<_>> = BTreeMap::new();
    for deleted in &file.deleted_lines {
        deleted_by_anchor
            .entry(deleted.anchor_line)
            .or_default()
            .push(deleted);
    }

    let mut rows = Vec::new();
    for line_index in 0..file.buffer.line_count() {
        if let Some(deleted_rows) = deleted_by_anchor.remove(&line_index) {
            rows.extend(deleted_rows.into_iter().map(|deleted| CodeRenderRow {
                kind: CodeRowKind::VirtualDeleted,
                buffer_line: None,
                lineno: deleted.old_lineno,
                text: deleted.text.clone(),
                change: Some(ChangeKind::Deleted),
            }));
        }

        rows.push(CodeRenderRow {
            kind: CodeRowKind::Buffer,
            buffer_line: Some(line_index),
            lineno: Some(line_index + 1),
            text: file.buffer.line(line_index).unwrap_or_default().to_owned(),
            change: file.line_change(line_index),
        });
    }

    if let Some(deleted_rows) = deleted_by_anchor.remove(&file.buffer.line_count()) {
        rows.extend(deleted_rows.into_iter().map(|deleted| CodeRenderRow {
            kind: CodeRowKind::VirtualDeleted,
            buffer_line: None,
            lineno: deleted.old_lineno,
            text: deleted.text.clone(),
            change: Some(ChangeKind::Deleted),
        }));
    }

    rows
}

fn rendered_code_view(app: &App, file: &ReviewFile, width: usize) -> RenderedCodeView {
    let rows = code_rows(file);
    let mut persisted_comment_boxes = BTreeMap::<usize, Vec<Vec<Line<'static>>>>::new();
    for comment in app
        .comments
        .iter()
        .filter(|comment| comment.file_path == file.display_path())
    {
        persisted_comment_boxes
            .entry(
                comment
                    .target
                    .end_line()
                    .min(file.buffer.line_count().saturating_sub(1)),
            )
            .or_default()
            .push(comment_box_lines(&comment.text, width, false));
    }
    let draft_comment_box = app
        .comment_draft()
        .zip(app.comment_box_anchor_line(file))
        .map(|(draft, anchor_line)| (comment_box_lines(draft, width, true), anchor_line));

    let mut lines = Vec::new();
    let mut cursor_visual_row = code_cursor_visual_row(&rows, app.code_cursor_line);

    for row in rows {
        let is_selected = row.buffer_line == Some(app.code_cursor_line);
        let in_selection = row
            .buffer_line
            .is_some_and(|line| app.line_in_selection(file, line));
        let has_comment = row
            .buffer_line
            .is_some_and(|line| app.line_has_comment(file.display_path(), line));
        if is_selected {
            cursor_visual_row = lines.len();
        }
        let row_buffer_line = row.buffer_line;
        let highlighted_line = row_buffer_line.and_then(|line| file.highlighted_line(line));
        let text_overlays =
            row_buffer_line.map_or_else(Vec::new, |line| code_text_overlays(app, file, line));
        lines.push(code_row_to_text(
            is_selected,
            in_selection,
            has_comment,
            highlighted_line,
            &text_overlays,
            &row,
            width,
        ));

        if let Some(anchor_line) = row_buffer_line {
            if let Some(comment_boxes) = persisted_comment_boxes.remove(&anchor_line) {
                for comment_box in comment_boxes {
                    lines.extend(comment_box);
                }
            }

            if let Some((comment_box_lines, draft_anchor_line)) = &draft_comment_box
                && anchor_line == *draft_anchor_line
            {
                lines.extend(comment_box_lines.iter().cloned());
            }
        }
    }

    RenderedCodeView {
        lines,
        cursor_visual_row,
    }
}

fn comment_box_lines(text: &str, width: usize, is_draft: bool) -> Vec<Line<'static>> {
    let text_indent = 0usize;
    let inner_width = width.saturating_sub(text_indent + 2).max(12);
    let horizontal = "─".repeat(inner_width);
    let border_style = Style::default().fg(Color::DarkGray);
    let title = if is_draft {
        " AI comment "
    } else {
        " Queued comment "
    };
    let top = if inner_width > title.len() {
        let remaining = inner_width - title.len();
        let left = remaining / 2;
        let right = remaining - left;
        format!(
            "{}┌{}{}{}┐",
            " ".repeat(text_indent),
            "─".repeat(left),
            title,
            "─".repeat(right)
        )
    } else {
        format!("{}┌{}┐", " ".repeat(text_indent), horizontal)
    };

    let wrapped = wrap_comment_text(
        if is_draft && text.is_empty() {
            "Type feedback for AI..."
        } else {
            text
        },
        inner_width,
    );
    let body_style = if is_draft && text.is_empty() {
        Style::default()
            .fg(Color::DarkGray)
            .add_modifier(Modifier::ITALIC)
    } else if is_draft {
        Style::default().fg(Color::Cyan)
    } else {
        Style::default().fg(Color::Yellow)
    };

    let mut lines = Vec::with_capacity(wrapped.len() + 2);
    lines.push(Line::styled(top, border_style));
    lines.extend(wrapped.into_iter().map(|segment| {
        Line::from(vec![
            Span::styled(format!("{}│", " ".repeat(text_indent)), border_style),
            Span::styled(format!("{segment:<inner_width$}"), body_style),
            Span::styled("│", border_style),
        ])
    }));
    lines.push(Line::styled(
        format!("{}└{}┘", " ".repeat(text_indent), horizontal),
        border_style,
    ));
    lines
}

fn wrap_comment_text(text: &str, width: usize) -> Vec<String> {
    let width = width.max(1);
    let mut wrapped = Vec::new();

    for paragraph in text.split('\n') {
        if paragraph.is_empty() {
            wrapped.push(String::new());
            continue;
        }

        let mut current = String::new();
        for word in paragraph.split_whitespace() {
            if current.is_empty() {
                push_wrapped_word(&mut wrapped, &mut current, word, width);
                continue;
            }

            if current.len() + 1 + word.len() <= width {
                current.push(' ');
                current.push_str(word);
            } else {
                wrapped.push(std::mem::take(&mut current));
                push_wrapped_word(&mut wrapped, &mut current, word, width);
            }
        }

        if !current.is_empty() {
            wrapped.push(current);
        }
    }

    if wrapped.is_empty() {
        wrapped.push(String::new());
    }

    wrapped
}

fn push_wrapped_word(wrapped: &mut Vec<String>, current: &mut String, word: &str, width: usize) {
    if word.len() <= width {
        current.push_str(word);
        return;
    }

    let mut start = 0;
    let chars = word.chars().collect::<Vec<_>>();
    while start < chars.len() {
        let end = (start + width).min(chars.len());
        let chunk = chars[start..end].iter().collect::<String>();
        if current.is_empty() {
            if end < chars.len() {
                wrapped.push(chunk);
            } else {
                current.push_str(&chunk);
            }
        } else {
            wrapped.push(std::mem::take(current));
            if end < chars.len() {
                wrapped.push(chunk);
            } else {
                current.push_str(&chunk);
            }
        }
        start = end;
    }
}

fn raw_rows(file: &ReviewFile) -> Vec<RawRenderRow> {
    if file.patch.has_binary_or_unrenderable_change && file.patch.hunks.is_empty() {
        return vec![RawRenderRow {
            kind: RawRowKind::Placeholder,
            old_lineno: None,
            new_lineno: None,
            text: "[binary or unrenderable diff]".to_owned(),
        }];
    }

    if file.patch.hunks.is_empty() {
        return vec![RawRenderRow {
            kind: RawRowKind::Placeholder,
            old_lineno: None,
            new_lineno: None,
            text: "[no diff hunks]".to_owned(),
        }];
    }

    let mut rows = Vec::new();
    for hunk in &file.patch.hunks {
        rows.push(RawRenderRow {
            kind: RawRowKind::HunkHeader,
            old_lineno: None,
            new_lineno: None,
            text: hunk.header.clone(),
        });
        rows.extend(hunk.lines.iter().map(|line| {
            let kind = match line.kind {
                frame_core::LineKind::Added => RawRowKind::Added,
                frame_core::LineKind::Removed => RawRowKind::Removed,
                frame_core::LineKind::Context => RawRowKind::Context,
            };
            RawRenderRow {
                kind,
                old_lineno: line.old_lineno,
                new_lineno: line.new_lineno,
                text: line.text.clone(),
            }
        }));
    }

    rows
}

fn raw_hunk_targets(file: &ReviewFile) -> Vec<usize> {
    raw_rows(file)
        .iter()
        .enumerate()
        .filter_map(|(index, row)| (row.kind == RawRowKind::HunkHeader).then_some(index))
        .collect()
}

fn raw_row_for_buffer_line(file: &ReviewFile, buffer_line: usize) -> usize {
    let target_lineno = buffer_line + 1;
    let rows = raw_rows(file);

    rows.iter()
        .position(|row| relevant_raw_lineno(file, row) == Some(target_lineno))
        .or_else(|| {
            rows.iter().position(|row| {
                relevant_raw_lineno(file, row).is_some_and(|lineno| lineno > target_lineno)
            })
        })
        .unwrap_or(0)
}

fn buffer_line_for_raw_row(file: &ReviewFile, row_index: usize) -> Option<usize> {
    let rows = raw_rows(file);

    rows.get(row_index)
        .and_then(|row| relevant_raw_lineno(file, row))
        .map(|lineno| lineno.saturating_sub(1))
        .or_else(|| {
            rows.iter()
                .skip(row_index)
                .find_map(|row| relevant_raw_lineno(file, row))
                .map(|lineno| lineno.saturating_sub(1))
        })
        .or_else(|| {
            rows.iter()
                .take(row_index)
                .rev()
                .find_map(|row| relevant_raw_lineno(file, row))
                .map(|lineno| lineno.saturating_sub(1))
        })
        .map(|line| line.min(file.buffer.line_count().saturating_sub(1)))
}

fn relevant_raw_lineno(file: &ReviewFile, row: &RawRenderRow) -> Option<usize> {
    match file.source {
        BufferSource::PostImage | BufferSource::Placeholder => row.new_lineno.or(row.old_lineno),
        BufferSource::PreImage => row.old_lineno.or(row.new_lineno),
    }
}

fn nth_next_target(targets: &[usize], current: usize, count: usize) -> Option<usize> {
    let mut current = current;
    let mut next = None;

    for _ in 0..count.max(1) {
        next = targets
            .iter()
            .copied()
            .find(|target| *target > current)
            .or_else(|| targets.last().copied());
        current = next?;
    }

    next
}

fn nth_previous_target(targets: &[usize], current: usize, count: usize) -> Option<usize> {
    let mut current = current;
    let mut next = None;

    for _ in 0..count.max(1) {
        next = targets
            .iter()
            .copied()
            .take_while(|target| *target < current)
            .last()
            .or_else(|| targets.first().copied());
        current = next?;
    }

    next
}

fn code_text_overlays(app: &App, file: &ReviewFile, line_index: usize) -> Vec<TextOverlay> {
    let mut overlays = Vec::new();
    let line_text = file.buffer.line(line_index).unwrap_or_default();

    if let Some(target) = app.selection_target(file)
        && let Some((start_byte, end_byte)) =
            target_segment_for_line(&target, line_index, line_text)
    {
        overlays.push(TextOverlay {
            start_byte,
            end_byte,
            style: selection_chunk_style(),
        });
    }

    for comment in app
        .comments
        .iter()
        .filter(|comment| comment.file_path == file.display_path())
    {
        if let Some((start_byte, end_byte)) =
            target_segment_for_line(&comment.target, line_index, line_text)
        {
            overlays.push(TextOverlay {
                start_byte,
                end_byte,
                style: comment_chunk_style(),
            });
        }
    }

    if let Some(chunk) = app.current_chunk(file)
        && chunk.span.start.line == line_index
    {
        overlays.push(TextOverlay {
            start_byte: chunk.span.start.byte_col.min(line_text.len()),
            end_byte: chunk.span.end.byte_col.min(line_text.len()),
            style: active_chunk_style(),
        });
    }

    overlays
}

fn target_segment_for_line(
    target: &CommentTarget,
    line_index: usize,
    line_text: &str,
) -> Option<(usize, usize)> {
    match target.normalized() {
        CommentTarget::ChunkSpan(span) => {
            if !span.intersects_line(line_index) {
                return None;
            }

            let start_byte = if line_index == span.start.line {
                span.start.byte_col.min(line_text.len())
            } else {
                0
            };
            let end_byte = if line_index == span.end.line {
                span.end.byte_col.min(line_text.len())
            } else {
                line_text.len()
            };

            (start_byte < end_byte).then_some((start_byte, end_byte))
        }
        CommentTarget::LineRange {
            start_line,
            end_line,
        } => (start_line <= line_index && line_index <= end_line && !line_text.is_empty())
            .then_some((0, line_text.len())),
    }
}

fn active_chunk_style() -> Style {
    Style::default()
        .bg(Color::Rgb(64, 64, 90))
        .add_modifier(Modifier::UNDERLINED | Modifier::BOLD)
}

fn selection_chunk_style() -> Style {
    Style::default().bg(Color::Rgb(52, 52, 72))
}

fn comment_chunk_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED)
}

fn format_comment_target(file_path: &str, target: &CommentTarget) -> String {
    match target.normalized() {
        CommentTarget::ChunkSpan(span) => {
            if span.start.line == span.end.line {
                format!(
                    "{file_path}:{}:{}-{}",
                    span.start.line + 1,
                    span.start.display_col + 1,
                    span.end.display_col + 1
                )
            } else {
                format!(
                    "{file_path}:{}:{}-{}:{}",
                    span.start.line + 1,
                    span.start.display_col + 1,
                    span.end.line + 1,
                    span.end.display_col + 1
                )
            }
        }
        CommentTarget::LineRange {
            start_line,
            end_line,
        } => {
            if start_line == end_line {
                format!("{file_path}:{}", start_line + 1)
            } else {
                format!("{file_path}:{}-{}", start_line + 1, end_line + 1)
            }
        }
    }
}

fn code_row_to_text(
    is_selected: bool,
    in_selection: bool,
    has_comment: bool,
    highlighted_line: Option<&HighlightedLine>,
    text_overlays: &[TextOverlay],
    row: &CodeRenderRow,
    width: usize,
) -> Line<'static> {
    let change_marker = match row.kind {
        CodeRowKind::VirtualDeleted => '-',
        CodeRowKind::Banner => '!',
        CodeRowKind::Buffer => match row.change {
            Some(ChangeKind::Added) => '+',
            Some(ChangeKind::Modified) => '~',
            Some(ChangeKind::Deleted) => 'x',
            None => ' ',
        },
    };
    let comment_marker = if has_comment { '!' } else { ' ' };
    let prefix = format!(
        "{:>4} {}{} ",
        format_lineno(row.lineno),
        change_marker,
        comment_marker,
    );
    let prefix_style = prefix_style(row, in_selection, has_comment, is_selected);
    let text_style = text_style(row, in_selection, has_comment, is_selected);

    let mut spans = vec![Span::styled(prefix, prefix_style)];
    match row.kind {
        CodeRowKind::Buffer => spans.extend(highlighted_text_spans(
            &row.text,
            highlighted_line,
            text_style,
            text_overlays,
        )),
        CodeRowKind::VirtualDeleted | CodeRowKind::Banner => {
            spans.push(Span::styled(row.text.clone(), text_style));
        }
    }

    if should_fill_code_row(row.change, in_selection, is_selected) {
        pad_spans_to_width(&mut spans, width, text_style);
    }

    Line::from(spans)
}

fn should_fill_code_row(change: Option<ChangeKind>, in_selection: bool, is_selected: bool) -> bool {
    overlay_background(change).is_some() || in_selection || is_selected
}

fn pad_spans_to_width(spans: &mut Vec<Span<'static>>, width: usize, style: Style) {
    let current_width = spans
        .iter()
        .map(|span| span.content.chars().count())
        .sum::<usize>();

    if current_width < width {
        spans.push(Span::styled(" ".repeat(width - current_width), style));
    }
}

fn prefix_style(
    row: &CodeRenderRow,
    in_selection: bool,
    has_comment: bool,
    is_selected: bool,
) -> Style {
    let accent = match row.kind {
        CodeRowKind::Buffer => match row.change {
            Some(ChangeKind::Added) => Color::Green,
            Some(ChangeKind::Modified) => Color::Yellow,
            Some(ChangeKind::Deleted) => Color::Red,
            None => Color::DarkGray,
        },
        CodeRowKind::VirtualDeleted => Color::Red,
        CodeRowKind::Banner => Color::Magenta,
    };

    apply_row_emphasis(
        Style::default().fg(accent),
        row.change,
        in_selection,
        has_comment,
        is_selected,
    )
}

fn text_style(
    row: &CodeRenderRow,
    in_selection: bool,
    has_comment: bool,
    is_selected: bool,
) -> Style {
    let base = match row.kind {
        CodeRowKind::Buffer => Style::default().fg(Color::Gray),
        CodeRowKind::VirtualDeleted => Style::default()
            .fg(Color::Red)
            .add_modifier(Modifier::ITALIC),
        CodeRowKind::Banner => Style::default()
            .fg(Color::Magenta)
            .add_modifier(Modifier::ITALIC),
    };

    apply_row_emphasis(base, row.change, in_selection, has_comment, is_selected)
}

fn apply_row_emphasis(
    mut style: Style,
    change: Option<ChangeKind>,
    in_selection: bool,
    has_comment: bool,
    is_selected: bool,
) -> Style {
    if let Some(color) = overlay_background(change) {
        style = style.bg(color);
    }

    if in_selection {
        style = style.bg(Color::DarkGray);
    }

    if has_comment {
        style = style.add_modifier(Modifier::BOLD);
    }

    if is_selected {
        style = style.add_modifier(Modifier::REVERSED);
    }

    style
}

fn overlay_background(change: Option<ChangeKind>) -> Option<Color> {
    match change {
        Some(ChangeKind::Added) => Some(Color::Rgb(12, 32, 20)),
        Some(ChangeKind::Modified) => Some(Color::Rgb(38, 34, 12)),
        Some(ChangeKind::Deleted) => Some(Color::Rgb(42, 18, 18)),
        None => None,
    }
}

fn highlighted_text_spans(
    text: &str,
    highlighted_line: Option<&HighlightedLine>,
    base_style: Style,
    text_overlays: &[TextOverlay],
) -> Vec<Span<'static>> {
    let mut boundaries = vec![0, text.len()];
    if let Some(highlighted_line) = highlighted_line {
        for span in &highlighted_line.spans {
            boundaries.push(span.start_byte.min(text.len()));
            boundaries.push(span.end_byte.min(text.len()));
        }
    }
    for overlay in text_overlays {
        boundaries.push(overlay.start_byte.min(text.len()));
        boundaries.push(overlay.end_byte.min(text.len()));
    }
    boundaries.sort_unstable();
    boundaries.dedup();

    let mut spans = Vec::new();
    for window in boundaries.windows(2) {
        let start = window[0];
        let end = window[1];
        if start >= end || !text.is_char_boundary(start) || !text.is_char_boundary(end) {
            continue;
        }

        let mut style = base_style;
        if let Some(style_key) = highlighted_line.and_then(|line| {
            line.spans
                .iter()
                .find(|span| span.start_byte <= start && end <= span.end_byte)
                .map(|span| span.style)
        }) {
            style = style.patch(syntax_style(style_key));
        }
        for overlay in text_overlays {
            if overlay.start_byte <= start && end <= overlay.end_byte {
                style = style.patch(overlay.style);
            }
        }

        spans.push(Span::styled(text[start..end].to_owned(), style));
    }

    if spans.is_empty() {
        spans.push(Span::styled(text.to_owned(), base_style));
    }

    spans
}

fn syntax_style(style: HighlightStyleKey) -> Style {
    match style {
        HighlightStyleKey::Attribute
        | HighlightStyleKey::PunctuationSpecial
        | HighlightStyleKey::Type
        | HighlightStyleKey::TypeBuiltin => Style::default().fg(Color::LightCyan),
        HighlightStyleKey::Comment => Style::default()
            .fg(Color::DarkGray)
            .add_modifier(Modifier::ITALIC),
        HighlightStyleKey::Constant | HighlightStyleKey::ConstantBuiltin => {
            Style::default().fg(Color::LightRed)
        }
        HighlightStyleKey::Constructor => Style::default().fg(Color::LightYellow),
        HighlightStyleKey::Embedded | HighlightStyleKey::Keyword => {
            Style::default().fg(Color::LightMagenta)
        }
        HighlightStyleKey::Function
        | HighlightStyleKey::FunctionBuiltin
        | HighlightStyleKey::Tag
        | HighlightStyleKey::TextReference
        | HighlightStyleKey::TextUri => Style::default().fg(Color::LightBlue),
        HighlightStyleKey::Module => Style::default().fg(Color::Cyan),
        HighlightStyleKey::Number => Style::default().fg(Color::LightRed),
        HighlightStyleKey::Operator
        | HighlightStyleKey::Variable
        | HighlightStyleKey::VariableBuiltin
        | HighlightStyleKey::VariableParameter => Style::default().fg(Color::White),
        HighlightStyleKey::Property | HighlightStyleKey::PropertyBuiltin => {
            Style::default().fg(Color::Cyan)
        }
        HighlightStyleKey::Punctuation
        | HighlightStyleKey::PunctuationBracket
        | HighlightStyleKey::PunctuationDelimiter => Style::default().fg(Color::Gray),
        HighlightStyleKey::String
        | HighlightStyleKey::StringEscape
        | HighlightStyleKey::StringSpecial
        | HighlightStyleKey::TextLiteral => Style::default().fg(Color::LightGreen),
        HighlightStyleKey::TextEmphasis => Style::default()
            .fg(Color::LightYellow)
            .add_modifier(Modifier::ITALIC),
        HighlightStyleKey::TextStrong => Style::default()
            .fg(Color::LightYellow)
            .add_modifier(Modifier::BOLD),
        HighlightStyleKey::TextTitle => Style::default()
            .fg(Color::LightMagenta)
            .add_modifier(Modifier::BOLD),
    }
}

fn raw_row_to_text(is_selected: bool, row: &RawRenderRow, width: usize) -> Line<'static> {
    let base_style = match row.kind {
        RawRowKind::HunkHeader => Style::default().fg(Color::Blue),
        RawRowKind::Added => Style::default().fg(Color::Green),
        RawRowKind::Removed => Style::default().fg(Color::Red),
        RawRowKind::Context => Style::default().fg(Color::DarkGray),
        RawRowKind::Placeholder => Style::default()
            .fg(Color::Magenta)
            .add_modifier(Modifier::ITALIC),
    };
    let mut style = base_style;
    if let Some(color) = raw_row_background(row.kind) {
        style = style.bg(color);
    }
    if is_selected {
        style = style.add_modifier(Modifier::REVERSED);
    }

    let marker = match row.kind {
        RawRowKind::Added => '+',
        RawRowKind::Removed => '-',
        RawRowKind::Placeholder => '!',
        _ => ' ',
    };

    let text = match row.kind {
        RawRowKind::HunkHeader | RawRowKind::Placeholder => row.text.clone(),
        RawRowKind::Added | RawRowKind::Removed | RawRowKind::Context => format!(
            "{:>4} {:>4} {} {}",
            format_lineno(row.old_lineno),
            format_lineno(row.new_lineno),
            marker,
            row.text
        ),
    };

    let mut spans = vec![Span::styled(text, style)];
    if raw_row_background(row.kind).is_some() || is_selected {
        pad_spans_to_width(&mut spans, width, style);
    }

    Line::from(spans)
}

fn raw_row_background(kind: RawRowKind) -> Option<Color> {
    match kind {
        RawRowKind::Added => Some(Color::Rgb(12, 32, 20)),
        RawRowKind::Removed => Some(Color::Rgb(42, 18, 18)),
        RawRowKind::HunkHeader | RawRowKind::Context | RawRowKind::Placeholder => None,
    }
}

fn format_lineno(lineno: Option<usize>) -> String {
    lineno.map_or_else(|| " ".to_owned(), |value| value.to_string())
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use frame_core::{
        BufferSource, FileChangeKind, LineKind, PatchFile, PatchHunk, PatchLine, ReviewFile,
        ReviewFileInput, ReviewSnapshot,
    };
    use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
    use ratatui::style::Color;

    use super::{
        App, CodeRowKind, CommentTarget, InputMode, MotionMode, RawRowKind, ViewMode, code_rows,
        comment_box_lines, raw_row_to_text, raw_rows, rendered_code_view,
    };

    fn key(code: KeyCode) -> KeyEvent {
        KeyEvent::new(code, KeyModifiers::NONE)
    }

    fn sample_main_file() -> ReviewFile {
        ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: Some("src/main.rs".to_owned()),
                new_path: Some("src/main.rs".to_owned()),
                change: FileChangeKind::Modified,
                hunks: vec![
                    PatchHunk {
                        header: "@@ -1,3 +1,4 @@".to_owned(),
                        old_start: 1,
                        old_len: 3,
                        new_start: 1,
                        new_len: 4,
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
                                kind: LineKind::Added,
                                old_lineno: None,
                                new_lineno: Some(3),
                                text: "    extra();".to_owned(),
                            },
                            PatchLine {
                                kind: LineKind::Context,
                                old_lineno: Some(3),
                                new_lineno: Some(4),
                                text: "}".to_owned(),
                            },
                        ],
                    },
                    PatchHunk {
                        header: "@@ -6,3 +7,3 @@".to_owned(),
                        old_start: 6,
                        old_len: 3,
                        new_start: 7,
                        new_len: 3,
                        lines: vec![
                            PatchLine {
                                kind: LineKind::Context,
                                old_lineno: Some(6),
                                new_lineno: Some(7),
                                text: "fn later() {".to_owned(),
                            },
                            PatchLine {
                                kind: LineKind::Removed,
                                old_lineno: Some(7),
                                new_lineno: None,
                                text: "    value();".to_owned(),
                            },
                            PatchLine {
                                kind: LineKind::Added,
                                old_lineno: None,
                                new_lineno: Some(8),
                                text: "    other();".to_owned(),
                            },
                            PatchLine {
                                kind: LineKind::Context,
                                old_lineno: Some(8),
                                new_lineno: Some(9),
                                text: "}".to_owned(),
                            },
                        ],
                    },
                ],
                has_binary_or_unrenderable_change: false,
            },
            buffer: frame_core::CodeBuffer::from_text(
                "fn main() {\n    new();\n    extra();\n}\n\n\nfn later() {\n    other();\n}\n",
            ),
            source: BufferSource::PostImage,
        })
    }

    fn sample_added_file() -> ReviewFile {
        ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: None,
                new_path: Some("src/lib.rs".to_owned()),
                change: FileChangeKind::Added,
                hunks: vec![PatchHunk {
                    header: "@@ -0,0 +1 @@".to_owned(),
                    old_start: 0,
                    old_len: 0,
                    new_start: 1,
                    new_len: 1,
                    lines: vec![PatchLine {
                        kind: LineKind::Added,
                        old_lineno: None,
                        new_lineno: Some(1),
                        text: "pub fn ready() {}".to_owned(),
                    }],
                }],
                has_binary_or_unrenderable_change: false,
            },
            buffer: frame_core::CodeBuffer::from_text("pub fn ready() {}\n"),
            source: BufferSource::PostImage,
        })
    }

    fn sample_snapshot() -> ReviewSnapshot {
        ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_main_file(), sample_added_file()],
        }
    }

    #[test]
    fn code_rows_insert_virtual_deleted_lines() {
        let snapshot = sample_snapshot();
        let rows = code_rows(&snapshot.files[0]);

        assert_eq!(rows[1].kind, CodeRowKind::VirtualDeleted);
        assert_eq!(rows[1].text, "    old();");
        assert_eq!(rows[2].kind, CodeRowKind::Buffer);
        assert_eq!(rows[2].buffer_line, Some(1));
    }

    #[test]
    fn app_navigates_between_changes_and_files() {
        let mut app = App::new(sample_snapshot());

        assert_eq!(app.code_cursor_line, 1);
        app.jump_next_change(1);
        assert_eq!(app.code_cursor_line, 7);
        app.jump_next_file(1);
        assert_eq!(app.active_file_index, 1);
        assert_eq!(app.code_cursor_line, 0);
    }

    #[test]
    fn app_supports_command_and_comment_input() {
        let mut app = App::new(sample_snapshot());

        assert!(!app.handle_key(key(KeyCode::Char(':'))));
        assert!(matches!(app.input_mode, InputMode::Command(_)));
        assert!(!app.handle_key(key(KeyCode::Char('d'))));
        assert!(!app.handle_key(key(KeyCode::Char('i'))));
        assert!(!app.handle_key(key(KeyCode::Char('f'))));
        assert!(!app.handle_key(key(KeyCode::Char('f'))));
        assert!(!app.handle_key(key(KeyCode::Enter)));
        assert_eq!(app.view_mode, ViewMode::RawDiff);

        assert!(!app.handle_key(key(KeyCode::Char('i'))));
        assert!(matches!(app.input_mode, InputMode::Comment(_)));
        assert!(!app.handle_key(key(KeyCode::Char('n'))));
        assert!(!app.handle_key(key(KeyCode::Char('i'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert!(!app.handle_key(key(KeyCode::Enter)));
        assert_eq!(app.comments.len(), 1);
    }

    #[test]
    fn app_toggles_file_explorer_with_e() {
        let mut app = App::new(sample_snapshot());

        assert!(app.file_explorer_open);
        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert!(!app.file_explorer_open);
        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert!(app.file_explorer_open);
    }

    #[test]
    fn app_toggles_visual_mode_with_v() {
        let mut app = App::new(sample_snapshot());

        assert_eq!(app.motion_mode, MotionMode::Normal);
        assert!(!app.handle_key(key(KeyCode::Char('v'))));
        assert_eq!(app.motion_mode, MotionMode::Visual);
        assert!(matches!(
            app.selection_target(&app.snapshot.files[0]),
            Some(CommentTarget::ChunkSpan(span)) if span.start.line == 1 && span.end.line == 1
        ));
        assert!(!app.handle_key(key(KeyCode::Char('j'))));
        assert!(matches!(
            app.selection_target(&app.snapshot.files[0]),
            Some(CommentTarget::ChunkSpan(span)) if span.start.line == 1 && span.end.line == 2
        ));
        assert!(!app.handle_key(key(KeyCode::Esc)));
        assert_eq!(app.motion_mode, MotionMode::Normal);
        assert_eq!(app.selection_target(&app.snapshot.files[0]), None);
    }

    #[test]
    fn visual_mode_comment_captures_selected_range() {
        let mut app = App::new(sample_snapshot());

        assert!(!app.handle_key(key(KeyCode::Char('v'))));
        assert!(!app.handle_key(key(KeyCode::Char('j'))));
        assert!(!app.handle_key(key(KeyCode::Char('i'))));
        assert!(matches!(app.input_mode, InputMode::Comment(_)));
        assert!(!app.handle_key(key(KeyCode::Char('n'))));
        assert!(!app.handle_key(key(KeyCode::Char('o'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert!(!app.handle_key(key(KeyCode::Enter)));
        assert_eq!(app.comments.len(), 1);
        assert!(matches!(
            app.comments[0].target,
            CommentTarget::ChunkSpan(span) if span.start.line == 1 && span.end.line == 2
        ));
        assert_eq!(app.motion_mode, MotionMode::Normal);
    }

    #[test]
    fn h_and_l_move_between_chunks() {
        let mut app = App::new(sample_snapshot());

        app.code_cursor_line = 0;
        app.set_code_cursor_to_first_chunk(0);
        let initial_chunk = app.code_cursor_chunk.expect("cursor starts on a chunk");
        assert!(
            app.snapshot.files[0]
                .chunks
                .line(0)
                .is_some_and(|line| line.chunks.len() >= 2)
        );
        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert_eq!(app.code_cursor_chunk, Some(initial_chunk + 1));
        assert!(!app.handle_key(key(KeyCode::Char('h'))));
        assert_eq!(app.code_cursor_chunk, Some(initial_chunk));
    }

    #[test]
    fn l_wraps_to_next_line_when_reaching_end_of_line() {
        let mut app = App::new(sample_snapshot());

        app.code_cursor_line = 0;
        app.set_code_cursor_to_last_chunk(0);

        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert_eq!(app.code_cursor_line, 1);
        assert_eq!(app.code_cursor_chunk, Some(0));
    }

    #[test]
    fn h_wraps_to_previous_line_when_reaching_start_of_line() {
        let mut app = App::new(sample_snapshot());

        assert_eq!(app.code_cursor_line, 1);
        assert_eq!(app.code_cursor_chunk, Some(0));

        assert!(!app.handle_key(key(KeyCode::Char('h'))));
        assert_eq!(app.code_cursor_line, 0);
        assert_eq!(
            app.code_cursor_chunk,
            app.snapshot.files[0].chunks.last_chunk_index(0)
        );
    }

    #[test]
    fn l_skips_lines_without_chunks_when_wrapping() {
        let mut app = App::new(sample_snapshot());

        app.code_cursor_line = 2;
        app.set_code_cursor_to_last_chunk(2);

        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert_eq!(app.code_cursor_line, 6);
        assert_eq!(app.code_cursor_chunk, Some(0));
    }

    #[test]
    fn count_prefix_moves_multiple_chunks() {
        let mut app = App::new(sample_snapshot());

        app.set_active_file(1);
        assert!(
            app.snapshot.files[1]
                .chunks
                .line(0)
                .is_some_and(|line| line.chunks.len() >= 3)
        );
        assert!(!app.handle_key(key(KeyCode::Char('2'))));
        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert_eq!(app.code_cursor_chunk, Some(2));
    }

    #[test]
    fn l_is_noop_when_no_next_chunk_exists() {
        let mut app = App::new(sample_snapshot());

        app.code_cursor_line = 7;
        app.set_code_cursor_to_last_chunk(7);

        assert_eq!(app.code_cursor_line, 7);
        assert_eq!(app.code_cursor_chunk, Some(0));
        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert_eq!(app.code_cursor_line, 7);
        assert_eq!(app.code_cursor_chunk, Some(0));
    }

    #[test]
    fn h_is_noop_when_no_previous_chunk_exists() {
        let mut app = App::new(sample_snapshot());

        app.code_cursor_line = 0;
        app.set_code_cursor_to_first_chunk(0);

        assert_eq!(app.code_cursor_line, 0);
        assert_eq!(app.code_cursor_chunk, Some(0));
        assert!(!app.handle_key(key(KeyCode::Char('h'))));
        assert_eq!(app.code_cursor_line, 0);
        assert_eq!(app.code_cursor_chunk, Some(0));
    }

    #[test]
    fn comment_box_wraps_long_ai_feedback() {
        let lines = comment_box_lines(
            "This is a long AI comment that should wrap inside the inline box.",
            30,
            true,
        );

        assert!(lines.len() > 3);
        assert!(lines[0].to_string().starts_with('┌'));
        assert!(lines[1].to_string().contains('│'));
        assert!(
            lines
                .last()
                .expect("box has a bottom")
                .to_string()
                .contains('└')
        );
    }

    #[test]
    fn comment_box_keeps_borders_neutral() {
        let lines = comment_box_lines("hello", 24, true);
        let body = lines.get(1).expect("box has a body line");

        assert_eq!(body.spans.len(), 3);
        assert_eq!(body.spans[0].style.fg, Some(Color::DarkGray));
        assert_eq!(body.spans[1].style.fg, Some(Color::Cyan));
        assert_eq!(body.spans[2].style.fg, Some(Color::DarkGray));
        assert_eq!(body.to_string(), "│hello                 │");
    }

    #[test]
    fn rendered_code_view_keeps_saved_comments_expanded() {
        let mut app = App::new(sample_snapshot());
        app.comments.push(super::ReviewComment {
            file_path: "src/main.rs".to_owned(),
            target: CommentTarget::LineRange {
                start_line: 1,
                end_line: 1,
            },
            text: "Keep this visible".to_owned(),
        });

        let rendered = rendered_code_view(&app, &app.snapshot.files[0], 36);
        let lines = rendered
            .lines
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>();

        assert!(lines.iter().any(|line| line.starts_with('┌')));
        assert!(lines.iter().any(|line| line.contains("Keep this visible")));
    }

    #[test]
    fn rendered_code_view_keeps_code_prefix_and_syntax_spans_separate() {
        let app = App::new(sample_snapshot());
        let rendered = rendered_code_view(&app, &app.snapshot.files[0], 80);
        let first_line = rendered.lines.first().expect("first line exists");

        assert!(first_line.spans.len() > 1);
        assert_eq!(first_line.spans[0].content.as_ref(), "   1    ");
        assert_eq!(
            first_line
                .spans
                .iter()
                .skip(1)
                .map(|span| span.content.as_ref())
                .collect::<String>(),
            "fn main() {"
        );
    }

    #[test]
    fn selected_code_line_pads_to_viewport_width() {
        let app = App::new(sample_snapshot());
        let rendered = rendered_code_view(&app, &app.snapshot.files[0], 30);
        let selected = rendered
            .lines
            .get(rendered.cursor_visual_row)
            .expect("selected line exists");

        assert_eq!(selected.to_string().chars().count(), 30);
    }

    #[test]
    fn changed_code_line_pads_to_viewport_width() {
        let app = App::new(sample_snapshot());
        let rendered = rendered_code_view(&app, &app.snapshot.files[0], 34);
        let changed = rendered
            .lines
            .iter()
            .find(|line| line.to_string().contains("extra();"))
            .expect("changed line exists");

        assert_eq!(changed.to_string().chars().count(), 34);
    }

    #[test]
    fn selected_raw_diff_line_pads_to_viewport_width() {
        let file = sample_main_file();
        let rows = raw_rows(&file);
        let selected = raw_row_to_text(true, &rows[0], 32);

        assert_eq!(selected.to_string().chars().count(), 32);
    }

    #[test]
    fn changed_raw_diff_line_pads_to_viewport_width() {
        let file = sample_main_file();
        let rows = raw_rows(&file);
        let added = rows
            .iter()
            .find(|row| matches!(row.kind, RawRowKind::Added))
            .expect("added row exists");
        let rendered = raw_row_to_text(false, added, 36);

        assert_eq!(rendered.to_string().chars().count(), 36);
    }
}
