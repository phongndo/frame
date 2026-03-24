use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, Sender},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use frame_core::{
    BufferSource, ChangeKind, HighlightStyleKey, HighlightedLine, ReviewFile, ReviewSnapshot,
};
use notify::{EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use ratatui::{
    Frame, Terminal,
    backend::CrosstermBackend,
    crossterm::{
        cursor::Show,
        event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers},
        execute,
        terminal::{EnterAlternateScreen, LeaveAlternateScreen, disable_raw_mode, enable_raw_mode},
    },
    layout::{Constraint, Layout, Rect},
    style::{Color, Modifier, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph},
};
use thiserror::Error;
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

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
enum InteractionMode {
    Content,
    Explorer,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PendingSequence {
    None,
    G,
    Z,
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
    LineRange { start_line: usize, end_line: usize },
}

impl CommentTarget {
    fn normalized(&self) -> Self {
        let Self::LineRange {
            start_line,
            end_line,
        } = self;
        Self::LineRange {
            start_line: (*start_line).min(*end_line),
            end_line: (*start_line).max(*end_line),
        }
    }

    fn intersects_line(&self, line_index: usize) -> bool {
        let Self::LineRange {
            start_line,
            end_line,
        } = self.normalized();
        start_line <= line_index && line_index <= end_line
    }

    fn end_line(&self) -> usize {
        let Self::LineRange { end_line, .. } = self.normalized();
        end_line
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CursorAnchor {
    line: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ReviewComment {
    file_path: String,
    target: CommentTarget,
    text: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum SidebarNodePath {
    Directory(String),
    File(String),
}

impl SidebarNodePath {
    fn path(&self) -> &str {
        match self {
            Self::Directory(path) | Self::File(path) => path,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct SidebarFileStats {
    added: usize,
    removed: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum SidebarRowKind {
    Directory {
        expanded: bool,
    },
    File {
        file_index: usize,
        stats: SidebarFileStats,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SidebarRow {
    key: SidebarNodePath,
    parent_path: String,
    sort_name: String,
    depth: usize,
    kind: SidebarRowKind,
}

impl SidebarRow {
    fn path(&self) -> &str {
        self.key.path()
    }

    fn file_index(&self) -> Option<usize> {
        match self.kind {
            SidebarRowKind::Directory { .. } => None,
            SidebarRowKind::File { file_index, .. } => Some(file_index),
        }
    }

    fn directory_expanded(&self) -> Option<bool> {
        match self.kind {
            SidebarRowKind::Directory { expanded } => Some(expanded),
            SidebarRowKind::File { .. } => None,
        }
    }
}

#[derive(Debug, Default)]
struct SidebarTreeNode {
    directories: BTreeMap<String, SidebarTreeNode>,
    files: BTreeMap<String, usize>,
}

#[derive(Debug)]
enum AppEvent {
    Terminal(Event),
    TerminalError(io::Error),
    RefreshResult(Result<ReviewSnapshot, String>),
    AutoRefreshUnavailable(String),
}

#[derive(Debug)]
struct RefreshFilter {
    repo_root: PathBuf,
    git_dir: PathBuf,
    git_common_dir: PathBuf,
}

impl RefreshFilter {
    fn new(repo_root: PathBuf, git_dir: PathBuf, git_common_dir: PathBuf) -> Self {
        Self {
            repo_root,
            git_dir,
            git_common_dir,
        }
    }

    #[cfg(test)]
    fn should_refresh_path(&self, path: &Path) -> Result<bool, String> {
        if path.starts_with(&self.git_dir) {
            return Ok(Self::is_relevant_git_path(path, &self.git_dir));
        }

        if path.starts_with(&self.git_common_dir) {
            return Ok(Self::is_relevant_git_path(path, &self.git_common_dir));
        }

        if !path.starts_with(&self.repo_root) {
            return Ok(false);
        }

        if frame_git::is_path_git_ignored(&self.repo_root, path)
            .map_err(|error| format!("Auto-refresh ignore check failed: {error}"))?
        {
            return Ok(false);
        }

        Ok(true)
    }

    fn is_relevant_event_kind(kind: EventKind) -> bool {
        !matches!(kind, EventKind::Access(_))
    }

    fn queue_relevant_paths(
        &self,
        event: &notify::Event,
        pending_paths: &mut BTreeSet<PathBuf>,
        needs_rescan: &mut bool,
    ) -> Result<bool, String> {
        if !Self::is_relevant_event_kind(event.kind) {
            return Ok(false);
        }

        if event.need_rescan() || event.paths.is_empty() {
            *needs_rescan = true;
            return Ok(true);
        }

        let mut relevant_change = false;
        let mut worktree_paths = Vec::new();

        for path in &event.paths {
            if path.starts_with(&self.git_dir) {
                if Self::is_relevant_git_path(path, &self.git_dir) {
                    pending_paths.insert(path.clone());
                    relevant_change = true;
                }
                continue;
            }

            if path.starts_with(&self.git_common_dir) {
                if Self::is_relevant_git_path(path, &self.git_common_dir) {
                    pending_paths.insert(path.clone());
                    relevant_change = true;
                }
                continue;
            }

            if path.starts_with(&self.repo_root) {
                worktree_paths.push(path.clone());
            }
        }

        let ignored_paths = frame_git::ignored_paths(&self.repo_root, &worktree_paths)
            .map_err(|error| format!("Auto-refresh ignore check failed: {error}"))?;

        for path in worktree_paths {
            if !ignored_paths.contains(&path) {
                pending_paths.insert(path.clone());
                relevant_change = true;
            }
        }

        Ok(relevant_change)
    }

    fn is_relevant_git_path(path: &Path, root: &Path) -> bool {
        let Ok(relative_path) = path.strip_prefix(root) else {
            return false;
        };

        if relative_path.as_os_str().is_empty() {
            return false;
        }

        if matches!(
            relative_path
                .components()
                .next()
                .map(std::path::Component::as_os_str),
            Some(component) if component == "objects" || component == "logs" || component == "hooks"
        ) {
            return false;
        }

        relative_path == Path::new("HEAD")
            || relative_path == Path::new("index")
            || relative_path == Path::new("packed-refs")
            || relative_path == Path::new("info/exclude")
            || relative_path.starts_with("refs")
    }
}

#[derive(Debug)]
struct TerminalCleanupGuard {
    active: bool,
}

impl TerminalCleanupGuard {
    fn new() -> Self {
        Self { active: true }
    }

    fn disarm(&mut self) {
        self.active = false;
    }
}

impl Drop for TerminalCleanupGuard {
    fn drop(&mut self) {
        if !self.active {
            return;
        }

        let _ = disable_raw_mode();
        let _ = execute!(io::stdout(), LeaveAlternateScreen, Show);
    }
}

#[derive(Debug)]
struct App {
    snapshot: ReviewSnapshot,
    raw_row_cache: Vec<Vec<RawRenderRow>>,
    active_file_index: usize,
    file_explorer_open: bool,
    interaction_mode: InteractionMode,
    sidebar_cursor_row: usize,
    sidebar_viewport_top: usize,
    sidebar_height: usize,
    expanded_dirs: BTreeSet<String>,
    sidebar_row_cache: Vec<SidebarRow>,
    sidebar_file_order_cache: Vec<usize>,
    code_cursor_line: usize,
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
    auto_refresh_warning: Option<String>,
}

impl App {
    fn new(snapshot: ReviewSnapshot) -> Self {
        let expanded_dirs = sidebar_directory_paths(&snapshot);
        let raw_row_cache = snapshot.files.iter().map(raw_rows).collect();
        let sidebar_row_cache = build_sidebar_rows(&snapshot, &expanded_dirs);
        let sidebar_file_order_cache = sidebar_file_order(&snapshot);
        let mut app = Self {
            expanded_dirs,
            sidebar_row_cache,
            sidebar_file_order_cache,
            snapshot,
            raw_row_cache,
            active_file_index: 0,
            file_explorer_open: true,
            interaction_mode: InteractionMode::Content,
            sidebar_cursor_row: 0,
            sidebar_viewport_top: 0,
            sidebar_height: 1,
            code_cursor_line: 0,
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
            auto_refresh_warning: None,
        };
        app.reset_active_file_positions();
        app.sync_sidebar_cursor_to_active_file();
        app
    }

    fn rebuild_sidebar_caches(&mut self) {
        self.sidebar_row_cache = build_sidebar_rows(&self.snapshot, &self.expanded_dirs);
        self.sidebar_file_order_cache = sidebar_file_order(&self.snapshot);
    }

    fn apply_snapshot_refresh(&mut self, new_snapshot: ReviewSnapshot) {
        if new_snapshot == self.snapshot {
            return;
        }

        let previous_sidebar_key = self.current_sidebar_key();
        let previous_directory_paths = sidebar_directory_paths(&self.snapshot);
        let previously_expanded_dirs = self.expanded_dirs.clone();
        let active_file_path = self
            .active_file()
            .map(|file| file.display_path().to_owned());
        let previous_file_index = self.active_file_index;
        let previous_line = self.code_cursor_line;
        let cleared_comment_state =
            !self.comments.is_empty() || matches!(self.input_mode, InputMode::Comment(_));

        self.snapshot = new_snapshot;
        self.raw_row_cache = self.snapshot.files.iter().map(raw_rows).collect();
        let current_directory_paths = sidebar_directory_paths(&self.snapshot);
        self.expanded_dirs = current_directory_paths
            .iter()
            .filter(|path| {
                previously_expanded_dirs.contains(*path)
                    || !previous_directory_paths.contains(*path)
            })
            .cloned()
            .collect();
        self.rebuild_sidebar_caches();
        self.active_file_index = active_file_path
            .as_deref()
            .and_then(|file_path| {
                self.snapshot
                    .files
                    .iter()
                    .position(|file| file.display_path() == file_path)
            })
            .or_else(|| {
                (!self.snapshot.files.is_empty())
                    .then_some(previous_file_index.min(self.snapshot.files.len() - 1))
            })
            .unwrap_or(0);

        self.set_code_cursor_line(previous_line);
        self.raw_cursor_line = self
            .active_file()
            .zip(self.active_raw_rows())
            .map_or(0, |(file, rows)| {
                raw_row_for_buffer_line_in_rows(file, rows, self.code_cursor_line)
            });

        self.pending_sequence = PendingSequence::None;
        self.pending_count = None;
        self.clear_visual_mode();
        if matches!(self.input_mode, InputMode::Comment(_)) {
            self.input_mode = InputMode::Normal;
        }
        if self.interaction_mode == InteractionMode::Explorer {
            self.restore_sidebar_cursor(previous_sidebar_key.as_ref(), true);
        } else {
            self.sync_sidebar_cursor_to_active_file();
        }
        self.comments.clear();
        if cleared_comment_state {
            self.set_status("Auto-refreshed review snapshot. Cleared local comments.");
        } else {
            self.set_status("Auto-refreshed review snapshot.");
        }
    }

    fn active_file(&self) -> Option<&ReviewFile> {
        self.snapshot.files.get(self.active_file_index)
    }

    fn active_raw_rows(&self) -> Option<&[RawRenderRow]> {
        self.raw_row_cache
            .get(self.active_file_index)
            .map(Vec::as_slice)
    }

    fn sidebar_rows(&self) -> &[SidebarRow] {
        &self.sidebar_row_cache
    }

    fn sidebar_file_order(&self) -> &[usize] {
        &self.sidebar_file_order_cache
    }

    fn current_sidebar_key(&self) -> Option<SidebarNodePath> {
        self.sidebar_rows()
            .get(self.sidebar_cursor_row)
            .map(|row| row.key.clone())
    }

    fn set_status(&mut self, message: &str) {
        message.clone_into(&mut self.status_message);
    }

    fn set_auto_refresh_warning(&mut self, message: String) {
        self.auto_refresh_warning = Some(message);
    }

    fn clear_visual_mode(&mut self) {
        self.motion_mode = MotionMode::Normal;
        self.visual_anchor = None;
    }

    fn activate_file(&mut self, file_index: usize) {
        if self.snapshot.files.is_empty() {
            self.active_file_index = 0;
            self.reset_active_file_positions();
            return;
        }

        self.active_file_index = file_index.min(self.snapshot.files.len().saturating_sub(1));
        self.clear_visual_mode();
        self.reset_active_file_positions();
    }

    fn set_active_file(&mut self, file_index: usize) {
        self.activate_file(file_index);
        self.sync_sidebar_cursor_to_active_file();
    }

    fn set_active_file_from_sidebar(&mut self, file_index: usize) {
        if self.active_file_index != file_index {
            self.activate_file(file_index);
        }
    }

    fn enter_explorer_mode(&mut self) {
        self.file_explorer_open = true;
        self.interaction_mode = InteractionMode::Explorer;
        self.sync_sidebar_cursor_to_active_file();
        self.set_status("Explorer focused.");
    }

    fn exit_explorer_mode(&mut self) {
        if self.interaction_mode == InteractionMode::Explorer {
            self.interaction_mode = InteractionMode::Content;
            self.sync_sidebar_cursor_to_active_file();
            self.set_status("Returned to content.");
        }
    }

    fn sync_sidebar_cursor_to_active_file(&mut self) {
        let Some(file_path) = self
            .active_file()
            .map(|file| file.display_path().to_owned())
        else {
            self.sidebar_cursor_row = 0;
            self.sidebar_viewport_top = 0;
            return;
        };

        let rows = self.sidebar_rows();
        let target_index = sidebar_cursor_index_for_file(rows, &file_path)
            .unwrap_or_else(|| self.sidebar_cursor_row.min(rows.len().saturating_sub(1)));
        self.sidebar_cursor_row = target_index;
        self.sync_sidebar_viewport();
    }

    fn restore_sidebar_cursor(
        &mut self,
        previous_key: Option<&SidebarNodePath>,
        preview_selected_file: bool,
    ) {
        let rows = self.sidebar_rows();
        if rows.is_empty() {
            self.sidebar_cursor_row = 0;
            self.sidebar_viewport_top = 0;
            return;
        }

        let restored_index = previous_key
            .and_then(|key| sidebar_restore_index(rows, key))
            .or_else(|| {
                self.active_file().and_then(|file| {
                    rows.iter().position(
                        |row| matches!(&row.key, SidebarNodePath::File(path) if path == file.display_path()),
                    )
                })
            })
            .unwrap_or(0);

        let preview_file_index = preview_selected_file
            .then(|| rows[restored_index].file_index())
            .flatten();

        self.sidebar_cursor_row = restored_index;
        if let Some(file_index) = preview_file_index {
            self.set_active_file_from_sidebar(file_index);
        }
        self.sync_sidebar_viewport();
    }

    fn set_sidebar_size(&mut self, height: usize) {
        self.sidebar_height = height.max(1);
    }

    fn sync_sidebar_viewport(&mut self) {
        let row_count = self.sidebar_rows().len();
        if row_count == 0 {
            self.sidebar_cursor_row = 0;
            self.sidebar_viewport_top = 0;
            return;
        }

        self.sidebar_cursor_row = self.sidebar_cursor_row.min(row_count.saturating_sub(1));
        self.sidebar_viewport_top = sync_viewport_top(
            self.sidebar_viewport_top,
            self.sidebar_cursor_row,
            row_count,
            self.sidebar_height,
        );
    }

    fn set_sidebar_cursor(&mut self, row_index: usize) {
        let rows = self.sidebar_rows();
        if rows.is_empty() {
            self.sidebar_cursor_row = 0;
            self.sidebar_viewport_top = 0;
            return;
        }

        let target_index = row_index.min(rows.len().saturating_sub(1));
        let file_index = rows[target_index].file_index();

        self.sidebar_cursor_row = target_index;
        if let Some(file_index) = file_index {
            self.set_active_file_from_sidebar(file_index);
        }
        self.sync_sidebar_viewport();
    }

    fn move_sidebar_up(&mut self, count: usize) {
        self.set_sidebar_cursor(self.sidebar_cursor_row.saturating_sub(count));
    }

    fn move_sidebar_down(&mut self, count: usize) {
        let rows = self.sidebar_rows();
        if rows.is_empty() {
            return;
        }

        let target_index = self
            .sidebar_cursor_row
            .saturating_add(count)
            .min(rows.len().saturating_sub(1));
        self.set_sidebar_cursor(target_index);
    }

    fn move_sidebar_to_start(&mut self, count: Option<usize>) {
        self.set_sidebar_cursor(count.unwrap_or(1).saturating_sub(1));
    }

    fn move_sidebar_to_end(&mut self, count: Option<usize>) {
        let rows = self.sidebar_rows();
        if rows.is_empty() {
            return;
        }

        let target = count.map_or_else(
            || rows.len().saturating_sub(1),
            |value| value.saturating_sub(1).min(rows.len().saturating_sub(1)),
        );
        self.set_sidebar_cursor(target);
    }

    fn move_sidebar_half_page_down(&mut self, count: usize) {
        let step = self.sidebar_half_page_step().saturating_mul(count.max(1));
        self.move_sidebar_down(step);
    }

    fn move_sidebar_half_page_up(&mut self, count: usize) {
        let step = self.sidebar_half_page_step().saturating_mul(count.max(1));
        self.move_sidebar_up(step);
    }

    fn sidebar_half_page_step(&self) -> usize {
        (self.sidebar_height.max(1) / 2).max(1)
    }

    fn current_sidebar_row(&self) -> Option<SidebarRow> {
        self.sidebar_rows().get(self.sidebar_cursor_row).cloned()
    }

    fn expand_sidebar_directory(&mut self, path: &str) -> bool {
        let changed = self.expanded_dirs.insert(path.to_owned());
        if changed {
            self.rebuild_sidebar_caches();
        }
        changed
    }

    fn collapse_sidebar_directory(&mut self, path: &str) -> bool {
        let changed = self.expanded_dirs.remove(path);
        if changed {
            self.rebuild_sidebar_caches();
        }
        changed
    }

    fn toggle_sidebar_directory(&mut self, path: &str) {
        if !self.collapse_sidebar_directory(path) {
            let _ = self.expand_sidebar_directory(path);
        }
    }

    fn handle_sidebar_left(&mut self) {
        let Some(row) = self.current_sidebar_row() else {
            return;
        };

        if row.directory_expanded().is_some_and(|expanded| expanded) {
            let _ = self.collapse_sidebar_directory(row.path());
            self.sync_sidebar_viewport();
            return;
        }

        if row.parent_path.is_empty() {
            return;
        }

        let rows = self.sidebar_rows();
        if let Some(parent_index) = rows.iter().position(|candidate| {
            matches!(&candidate.key, SidebarNodePath::Directory(path) if path == &row.parent_path)
        }) {
            self.sidebar_cursor_row = parent_index;
            self.sync_sidebar_viewport();
        }
    }

    fn handle_sidebar_right(&mut self) {
        let Some(row) = self.current_sidebar_row() else {
            return;
        };

        match row.kind {
            SidebarRowKind::Directory { expanded } => {
                if !expanded {
                    let _ = self.expand_sidebar_directory(row.path());
                    self.sync_sidebar_viewport();
                    return;
                }

                let rows = self.sidebar_rows();
                if let Some(child_index) = sidebar_first_child_index(rows, self.sidebar_cursor_row)
                {
                    self.set_sidebar_cursor(child_index);
                }
            }
            SidebarRowKind::File { .. } => self.exit_explorer_mode(),
        }
    }

    fn handle_sidebar_enter(&mut self) {
        let Some(row) = self.current_sidebar_row() else {
            return;
        };

        match row.kind {
            SidebarRowKind::Directory { .. } => {
                self.toggle_sidebar_directory(row.path());
                self.sync_sidebar_viewport();
            }
            SidebarRowKind::File { .. } => self.exit_explorer_mode(),
        }
    }

    fn cursor_anchor(&self) -> CursorAnchor {
        CursorAnchor {
            line: self.code_cursor_line,
        }
    }

    fn selection_target(&self) -> Option<CommentTarget> {
        if self.view_mode != ViewMode::Code || self.motion_mode != MotionMode::Visual {
            return None;
        }

        let anchor = self.visual_anchor?;
        Some(CommentTarget::LineRange {
            start_line: anchor.line.min(self.code_cursor_line),
            end_line: anchor.line.max(self.code_cursor_line),
        })
    }

    fn line_in_selection(&self, line_index: usize) -> bool {
        self.selection_target()
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
            self.selection_target()
                .unwrap_or_else(|| self.current_comment_target(file))
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

        if self.interaction_mode == InteractionMode::Explorer {
            return self.handle_explorer_key(key);
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

    fn handle_explorer_key(&mut self, key: KeyEvent) -> bool {
        if key.modifiers.contains(KeyModifiers::CONTROL) {
            let count = self.take_count();
            self.pending_sequence = PendingSequence::None;

            match key.code {
                KeyCode::Char('d') => self.move_sidebar_half_page_down(count),
                KeyCode::Char('u') => self.move_sidebar_half_page_up(count),
                _ => {}
            }

            return false;
        }

        match key.code {
            KeyCode::Char('q') => return true,
            KeyCode::Char(ch) if ch.is_ascii_digit() => {
                if ch == '0' && self.pending_count.is_none() {
                    self.pending_sequence = PendingSequence::None;
                    self.move_sidebar_to_start(None);
                } else {
                    self.push_count_digit(ch);
                }
            }
            KeyCode::Esc => {
                self.clear_prefixes();
                self.exit_explorer_mode();
            }
            KeyCode::Char('e') => {
                self.clear_prefixes();
                self.toggle_file_explorer();
            }
            KeyCode::Char('j') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                self.move_sidebar_down(count);
            }
            KeyCode::Char('k') => {
                let count = self.take_count();
                self.pending_sequence = PendingSequence::None;
                self.move_sidebar_up(count);
            }
            KeyCode::Char('G') => {
                let count = self.pending_count.take();
                self.pending_sequence = PendingSequence::None;
                self.move_sidebar_to_end(count);
            }
            KeyCode::Char('g') => self.handle_sidebar_g_sequence(),
            KeyCode::Char('h') => {
                self.clear_prefixes();
                self.handle_sidebar_left();
            }
            KeyCode::Char('l') => {
                self.clear_prefixes();
                self.handle_sidebar_right();
            }
            KeyCode::Enter => {
                self.clear_prefixes();
                self.handle_sidebar_enter();
            }
            _ => self.clear_prefixes(),
        }

        false
    }

    fn handle_normal_key(&mut self, key: KeyEvent) -> bool {
        match key.code {
            KeyCode::Char('q') => return true,
            KeyCode::Char(ch) if ch.is_ascii_digit() => {
                if ch == '0' && self.pending_count.is_none() {
                    self.pending_sequence = PendingSequence::None;
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
            KeyCode::Char('G') => {
                let count = self.pending_count.take();
                self.pending_sequence = PendingSequence::None;
                self.move_to_end(count);
            }
            KeyCode::Char('g') => {
                self.handle_g_sequence();
            }
            KeyCode::Char('z') => {
                self.handle_z_prefix();
            }
            KeyCode::Char('t') => {
                self.handle_zt_sequence();
            }
            KeyCode::Char('d') => self.handle_d_sequence(),
            KeyCode::Char(']') => self.pending_sequence = PendingSequence::CloseBracket,
            KeyCode::Char('[') => self.pending_sequence = PendingSequence::OpenBracket,
            KeyCode::Char('c') => self.handle_change_sequence(),
            KeyCode::Char('f') => self.handle_file_sequence(),
            KeyCode::Char('h') => self.handle_hunk_sequence(),
            _ => {
                self.clear_prefixes();
            }
        }

        false
    }

    fn handle_sidebar_g_sequence(&mut self) {
        if self.pending_sequence == PendingSequence::G {
            let count = self.pending_count.take();
            self.move_sidebar_to_start(count);
            self.pending_sequence = PendingSequence::None;
        } else {
            self.pending_sequence = PendingSequence::G;
        }
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

    fn handle_z_prefix(&mut self) {
        self.pending_sequence = PendingSequence::Z;
    }

    fn handle_zt_sequence(&mut self) {
        let should_align = self.pending_sequence == PendingSequence::Z;
        self.clear_prefixes();
        if should_align {
            self.align_viewport_to_cursor_top();
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
                if let (Some(file), Some(rows)) = (self.active_file(), self.active_raw_rows()) {
                    self.raw_cursor_line =
                        raw_row_for_buffer_line_in_rows(file, rows, self.code_cursor_line);
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

        let Some((file_path, target)) = self.active_file().map(|file| {
            (
                file.display_path().to_owned(),
                self.selection_target()
                    .unwrap_or_else(|| self.current_comment_target(file)),
            )
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

    fn current_comment_target(&self, file: &ReviewFile) -> CommentTarget {
        let next_line = line_index_for_file(file, self.code_cursor_line);

        CommentTarget::LineRange {
            start_line: next_line,
            end_line: next_line,
        }
    }

    fn set_code_cursor_line(&mut self, line: usize) {
        let Some(next_line) = self
            .active_file()
            .map(|file| line_index_for_file(file, line))
        else {
            self.code_cursor_line = 0;
            return;
        };

        self.code_cursor_line = next_line;
    }

    fn move_up(&mut self, count: usize) {
        match self.view_mode {
            ViewMode::Code => {
                self.set_code_cursor_line(self.code_cursor_line.saturating_sub(count));
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
                self.set_code_cursor_line(
                    self.code_cursor_line.saturating_add(count).min(max_index),
                );
            }
            ViewMode::RawDiff => {
                let max_index = self
                    .active_raw_rows()
                    .map_or(0, |rows| rows.len().saturating_sub(1));
                self.raw_cursor_line = self.raw_cursor_line.saturating_add(count).min(max_index);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_to_start(&mut self, count: Option<usize>) {
        match self.view_mode {
            ViewMode::Code => {
                let target = count.unwrap_or(1).saturating_sub(1);
                self.set_code_cursor_line(target);
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
                self.set_code_cursor_line(target.saturating_sub(1));
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = count.map_or_else(
                    || {
                        self.active_raw_rows()
                            .map_or(0, |rows| rows.len().saturating_sub(1))
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
                self.set_code_cursor_line(
                    self.code_cursor_line.saturating_add(step).min(max_index),
                );
            }
            ViewMode::RawDiff => {
                let max_index = self
                    .active_raw_rows()
                    .map_or(0, |rows| rows.len().saturating_sub(1));
                self.raw_cursor_line = (self.raw_cursor_line + step).min(max_index);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_half_page_up(&mut self, count: usize) {
        let step = self.half_page_step().saturating_mul(count.max(1));

        match self.view_mode {
            ViewMode::Code => {
                self.set_code_cursor_line(self.code_cursor_line.saturating_sub(step));
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
                    self.set_code_cursor_line(target);
                }
            }
            ViewMode::RawDiff => {
                let (Some(file), Some(rows)) = (self.active_file(), self.active_raw_rows()) else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| raw_row_for_buffer_line_in_rows(file, rows, anchor.buffer_line))
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
                    self.set_code_cursor_line(target);
                }
            }
            ViewMode::RawDiff => {
                let (Some(file), Some(rows)) = (self.active_file(), self.active_raw_rows()) else {
                    return;
                };
                let targets = file
                    .anchors
                    .iter()
                    .map(|anchor| raw_row_for_buffer_line_in_rows(file, rows, anchor.buffer_line))
                    .collect::<Vec<_>>();
                if let Some(target) = nth_previous_target(&targets, self.raw_cursor_line, count) {
                    self.raw_cursor_line = target;
                    self.sync_code_cursor_from_raw();
                }
            }
        }
    }

    fn jump_next_file(&mut self, count: usize) {
        let file_order = self.sidebar_file_order();
        if file_order.is_empty() {
            return;
        }

        let current_index = file_order
            .iter()
            .position(|file_index| *file_index == self.active_file_index)
            .unwrap_or(0);
        let target_index = current_index
            .saturating_add(count.max(1))
            .min(file_order.len().saturating_sub(1));
        self.set_active_file(file_order[target_index]);
    }

    fn jump_previous_file(&mut self, count: usize) {
        let file_order = self.sidebar_file_order();
        if file_order.is_empty() {
            return;
        }

        let current_index = file_order
            .iter()
            .position(|file_index| *file_index == self.active_file_index)
            .unwrap_or(0);
        let target_index = current_index.saturating_sub(count.max(1));
        self.set_active_file(file_order[target_index]);
    }

    fn jump_next_hunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(rows) = self.active_raw_rows() else {
            return;
        };
        let targets = raw_hunk_targets_in_rows(rows);
        if let Some(target) = nth_next_target(&targets, self.raw_cursor_line, count) {
            self.raw_cursor_line = target;
            self.sync_code_cursor_from_raw();
        }
    }

    fn jump_previous_hunk(&mut self, count: usize) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(rows) = self.active_raw_rows() else {
            return;
        };
        let targets = raw_hunk_targets_in_rows(rows);
        if let Some(target) = nth_previous_target(&targets, self.raw_cursor_line, count) {
            self.raw_cursor_line = target;
            self.sync_code_cursor_from_raw();
        }
    }

    fn toggle_mode(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                if let (Some(file), Some(rows)) = (self.active_file(), self.active_raw_rows()) {
                    self.raw_cursor_line =
                        raw_row_for_buffer_line_in_rows(file, rows, self.code_cursor_line);
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
        self.clear_prefixes();

        if self.file_explorer_open && self.interaction_mode == InteractionMode::Explorer {
            self.file_explorer_open = false;
            self.interaction_mode = InteractionMode::Content;
            self.set_status("Explorer closed.");
        } else {
            self.enter_explorer_mode();
        }
    }

    fn half_page_step(&self) -> usize {
        (self.viewport_height.max(1) / 2).max(1)
    }

    fn reset_active_file_positions(&mut self) {
        let (code_cursor, raw_cursor) = if let Some(file) = self.active_file() {
            let code_cursor = first_anchor_line(file);
            let raw_cursor = self.active_raw_rows().map_or(0, |rows| {
                raw_row_for_buffer_line_in_rows(file, rows, code_cursor)
            });
            (code_cursor, raw_cursor)
        } else {
            (0, 0)
        };

        self.code_cursor_line = code_cursor;
        self.code_viewport_top = 0;
        self.raw_cursor_line = raw_cursor;
        self.raw_viewport_top = 0;
    }

    fn sync_code_cursor_from_raw(&mut self) {
        let new_cursor = self
            .active_file()
            .zip(self.active_raw_rows())
            .and_then(|(file, rows)| {
                buffer_line_for_raw_row_in_rows(file, rows, self.raw_cursor_line)
            })
            .unwrap_or(self.code_cursor_line);
        self.set_code_cursor_line(new_cursor);
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
        let Some(rows) = self.active_raw_rows() else {
            self.raw_viewport_top = 0;
            return;
        };
        self.raw_viewport_top = sync_viewport_top(
            self.raw_viewport_top,
            self.raw_cursor_line,
            rows.len(),
            self.viewport_height,
        );
    }

    fn align_viewport_to_cursor_top(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                let Some(file) = self.active_file() else {
                    self.code_viewport_top = 0;
                    return;
                };
                let rendered = rendered_code_view(self, file, self.viewport_width);
                self.code_viewport_top = top_aligned_viewport_top(
                    rendered.cursor_visual_row,
                    rendered.lines.len(),
                    self.viewport_height,
                );
            }
            ViewMode::RawDiff => {
                let Some(rows) = self.active_raw_rows() else {
                    self.raw_viewport_top = 0;
                    return;
                };
                self.raw_viewport_top = top_aligned_viewport_top(
                    self.raw_cursor_line,
                    rows.len(),
                    self.viewport_height,
                );
            }
        }
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
        let normal_status = self.auto_refresh_warning.as_ref().map_or_else(
            || self.status_message.clone(),
            |warning| format!("{warning} | {}", self.status_message),
        );

        match &self.input_mode {
            InputMode::Normal => match self.interaction_mode {
                InteractionMode::Content => format!(
                    "{}{} | {} | {} queued | v visual | e explorer | : commands | i comment | gd/tab toggle | [c/]c change | [f/]f file",
                    count_prefix,
                    self.mode_label(),
                    normal_status,
                    self.comments.len()
                ),
                InteractionMode::Explorer => format!(
                    "{count_prefix}EXPLORER | {normal_status} | enter confirm | esc content | h/l tree | j/k move | e close"
                ),
            },
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
    let mut cleanup_guard = TerminalCleanupGuard::new();
    enable_raw_mode()?;
    execute!(stdout, EnterAlternateScreen)?;

    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;
    let loop_result = run_loop(&mut terminal, App::new(snapshot));
    let restore_result = restore_terminal(&mut terminal);

    if loop_result.is_ok() && restore_result.is_ok() {
        cleanup_guard.disarm();
    }

    loop_result.and(restore_result)
}

fn run_loop(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
    mut app: App,
) -> Result<(), ViewError> {
    let repo_root = app.snapshot.repo_root.clone();
    let (app_event_tx, app_event_rx) = mpsc::channel();
    let shutdown = Arc::new(AtomicBool::new(false));

    let input_handle = spawn_input_thread(app_event_tx.clone(), Arc::clone(&shutdown));
    let refresh_handle =
        match spawn_refresh_thread(&repo_root, app_event_tx.clone(), Arc::clone(&shutdown)) {
            Ok(handle) => Some(handle),
            Err(error) => {
                app.set_auto_refresh_warning(format!("Auto-refresh unavailable: {error}"));
                None
            }
        };
    drop(app_event_tx);

    terminal.draw(|frame| render(frame, &mut app))?;

    let loop_result = loop {
        let app_event = app_event_rx.recv().map_err(|_| {
            io::Error::new(
                io::ErrorKind::BrokenPipe,
                "frame event loop closed unexpectedly",
            )
        })?;

        let should_draw = match app_event {
            AppEvent::Terminal(Event::Key(key)) => {
                if app.handle_key(key) {
                    break Ok(());
                }
                true
            }
            AppEvent::Terminal(Event::Resize(_, _)) => true,
            AppEvent::Terminal(_) => false,
            AppEvent::TerminalError(error) => break Err(ViewError::Io(error)),
            AppEvent::RefreshResult(result) => {
                match result {
                    Ok(snapshot) => app.apply_snapshot_refresh(snapshot),
                    Err(message) => app.set_status(&message),
                }
                true
            }
            AppEvent::AutoRefreshUnavailable(message) => {
                app.set_auto_refresh_warning(message);
                true
            }
        };

        if should_draw {
            terminal.draw(|frame| render(frame, &mut app))?;
        }
    };

    shutdown.store(true, Ordering::Relaxed);
    drop(app_event_rx);
    join_thread(input_handle, "input")?;
    if let Some(handle) = refresh_handle {
        join_thread(handle, "refresh")?;
    }

    loop_result
}

fn join_thread(handle: JoinHandle<()>, name: &str) -> Result<(), ViewError> {
    handle
        .join()
        .map_err(|_| io::Error::other(format!("frame {name} thread panicked")))?;
    Ok(())
}

fn spawn_input_thread(app_event_tx: Sender<AppEvent>, shutdown: Arc<AtomicBool>) -> JoinHandle<()> {
    thread::spawn(move || {
        while !shutdown.load(Ordering::Relaxed) {
            match event::poll(Duration::from_millis(50)) {
                Ok(true) => match event::read() {
                    Ok(event) => {
                        if app_event_tx.send(AppEvent::Terminal(event)).is_err() {
                            break;
                        }
                    }
                    Err(error) => {
                        let _ = app_event_tx.send(AppEvent::TerminalError(error));
                        break;
                    }
                },
                Err(error) => {
                    let _ = app_event_tx.send(AppEvent::TerminalError(error));
                    break;
                }
                Ok(false) => {}
            }
        }
    })
}

fn spawn_refresh_thread(
    repo_root: &Path,
    app_event_tx: Sender<AppEvent>,
    shutdown: Arc<AtomicBool>,
) -> Result<JoinHandle<()>, String> {
    let repo_root = repo_root.to_path_buf();
    let git_dir =
        frame_git::resolve_git_dir_from_dir(&repo_root).map_err(|error| error.to_string())?;
    let git_common_dir = frame_git::resolve_git_common_dir_from_dir(&repo_root)
        .map_err(|error| error.to_string())?;
    let filter = RefreshFilter::new(repo_root.clone(), git_dir.clone(), git_common_dir.clone());
    let (watch_tx, watch_rx) = mpsc::channel();
    let mut watcher = notify::recommended_watcher(watch_tx).map_err(|error| error.to_string())?;

    let mut watched_roots = Vec::new();
    for path in [&repo_root, &git_dir, &git_common_dir] {
        watch_path_if_needed(&mut watcher, &mut watched_roots, path)
            .map_err(|error| error.to_string())?;
    }

    Ok(thread::spawn(move || {
        run_refresh_loop(
            &repo_root,
            &filter,
            &watch_rx,
            &app_event_tx,
            shutdown.as_ref(),
            watcher,
        );
    }))
}

fn watch_path_if_needed(
    watcher: &mut RecommendedWatcher,
    watched_roots: &mut Vec<PathBuf>,
    path: &Path,
) -> notify::Result<()> {
    if watched_roots.iter().any(|root| path.starts_with(root)) {
        return Ok(());
    }

    watcher.watch(path, RecursiveMode::Recursive)?;
    watched_roots.push(path.to_path_buf());
    Ok(())
}

fn run_refresh_loop(
    repo_root: &Path,
    filter: &RefreshFilter,
    watch_rx: &Receiver<notify::Result<notify::Event>>,
    app_event_tx: &Sender<AppEvent>,
    shutdown: &AtomicBool,
    _watcher: RecommendedWatcher,
) {
    const DEBOUNCE: Duration = Duration::from_millis(100);
    const SHUTDOWN_POLL: Duration = Duration::from_millis(50);

    let mut pending_paths = BTreeSet::new();
    let mut needs_rescan = false;
    let mut refresh_deadline: Option<Instant> = None;

    loop {
        if shutdown.load(Ordering::Relaxed) {
            break;
        }

        if let Some(deadline) = refresh_deadline {
            let now = Instant::now();
            if now >= deadline {
                if let Some(refresh_result) =
                    take_refresh_action(repo_root, &mut pending_paths, &mut needs_rescan)
                    && app_event_tx
                        .send(AppEvent::RefreshResult(refresh_result))
                        .is_err()
                {
                    break;
                }
                refresh_deadline = None;
                continue;
            }
        }

        let timeout = refresh_deadline.map_or(SHUTDOWN_POLL, |deadline| {
            deadline
                .saturating_duration_since(Instant::now())
                .min(SHUTDOWN_POLL)
        });

        let result = match watch_rx.recv_timeout(timeout) {
            Ok(result) => Some(result),
            Err(RecvTimeoutError::Timeout) => None,
            Err(RecvTimeoutError::Disconnected) => break,
        };

        let Some(result) = result else {
            continue;
        };

        match result {
            Ok(event) => {
                match filter.queue_relevant_paths(&event, &mut pending_paths, &mut needs_rescan) {
                    Ok(true) => {
                        refresh_deadline = Some(Instant::now() + DEBOUNCE);
                    }
                    Ok(false) => {}
                    Err(message) => {
                        if app_event_tx
                            .send(AppEvent::RefreshResult(Err(message)))
                            .is_err()
                        {
                            break;
                        }
                    }
                }
            }
            Err(error) => {
                if app_event_tx
                    .send(AppEvent::AutoRefreshUnavailable(format!(
                        "Auto-refresh unavailable: watcher failed: {error}",
                    )))
                    .is_err()
                {
                    break;
                }
                break;
            }
        }
    }
}

fn take_refresh_action(
    repo_root: &Path,
    pending_paths: &mut BTreeSet<PathBuf>,
    needs_rescan: &mut bool,
) -> Option<Result<ReviewSnapshot, String>> {
    let should_refresh = *needs_rescan || !pending_paths.is_empty();
    pending_paths.clear();
    *needs_rescan = false;

    if should_refresh {
        Some(
            frame_git::load_review_snapshot_from_dir(repo_root)
                .map_err(|error| format!("Auto-refresh failed: {error}")),
        )
    } else {
        None
    }
}

fn restore_terminal(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
) -> Result<(), ViewError> {
    let raw_mode_result = disable_raw_mode();
    let alternate_screen_result = execute!(terminal.backend_mut(), LeaveAlternateScreen);
    let show_cursor_result = terminal.show_cursor();

    raw_mode_result?;
    alternate_screen_result?;
    show_cursor_result?;
    Ok(())
}

fn render(frame: &mut Frame<'_>, app: &mut App) {
    let vertical =
        Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).split(frame.area());
    let content_area = if app.file_explorer_open {
        let layout =
            Layout::horizontal([Constraint::Length(40), Constraint::Min(10)]).split(vertical[0]);
        render_sidebar(frame, app, layout[0]);
        layout[1]
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
                let rows = app.active_raw_rows().unwrap_or(&[]);
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

impl SidebarTreeNode {
    fn insert_file(&mut self, path: &str, file_index: usize) {
        let segments = path
            .split('/')
            .filter(|segment| !segment.is_empty())
            .collect::<Vec<_>>();
        let Some((file_name, directories)) = segments.split_last() else {
            return;
        };

        let mut node = self;
        for directory in directories {
            node = node.directories.entry((*directory).to_owned()).or_default();
        }
        node.files.insert((*file_name).to_owned(), file_index);
    }
}

fn render_sidebar(frame: &mut Frame<'_>, app: &mut App, area: Rect) {
    let block = Block::default().borders(Borders::ALL).title("Files");
    let inner = block.inner(area);
    frame.render_widget(block, area);

    if inner.width == 0 || inner.height == 0 {
        return;
    }

    app.set_sidebar_size(inner.height as usize);
    app.sync_sidebar_viewport();
    let rows = app.sidebar_rows();
    let lines = if rows.is_empty() {
        vec![Line::styled(
            pad_display_text("No changed files", inner.width as usize),
            Style::default()
                .fg(Color::DarkGray)
                .add_modifier(Modifier::ITALIC),
        )]
    } else {
        rows.iter()
            .enumerate()
            .skip(app.sidebar_viewport_top)
            .take(inner.height as usize)
            .map(|(index, row)| sidebar_row_to_text(app, row, index, inner.width as usize))
            .collect()
    };

    frame.render_widget(Paragraph::new(lines), inner);
}

fn sidebar_directory_paths(snapshot: &ReviewSnapshot) -> BTreeSet<String> {
    let mut directories = BTreeSet::new();
    for file in &snapshot.files {
        let segments = file
            .display_path()
            .split('/')
            .filter(|segment| !segment.is_empty())
            .collect::<Vec<_>>();
        let mut current = String::new();
        for directory in segments.iter().take(segments.len().saturating_sub(1)) {
            if !current.is_empty() {
                current.push('/');
            }
            current.push_str(directory);
            directories.insert(current.clone());
        }
    }

    directories
}

fn sidebar_parent_paths(path: &str) -> Vec<String> {
    let segments = path
        .split('/')
        .filter(|segment| !segment.is_empty())
        .collect::<Vec<_>>();
    let mut parents = Vec::new();
    let mut current = String::new();
    for directory in segments.iter().take(segments.len().saturating_sub(1)) {
        if !current.is_empty() {
            current.push('/');
        }
        current.push_str(directory);
        parents.push(current.clone());
    }

    parents
}

fn build_sidebar_rows(
    snapshot: &ReviewSnapshot,
    expanded_dirs: &BTreeSet<String>,
) -> Vec<SidebarRow> {
    let mut tree = SidebarTreeNode::default();
    for (file_index, file) in snapshot.files.iter().enumerate() {
        tree.insert_file(file.display_path(), file_index);
    }

    let mut rows = Vec::new();
    append_sidebar_rows("", 0, &tree, expanded_dirs, snapshot, &mut rows);
    rows
}

fn append_sidebar_rows(
    parent_path: &str,
    depth: usize,
    tree: &SidebarTreeNode,
    expanded_dirs: &BTreeSet<String>,
    snapshot: &ReviewSnapshot,
    rows: &mut Vec<SidebarRow>,
) {
    for (directory_name, child) in &tree.directories {
        let path = join_sidebar_path(parent_path, directory_name);
        let expanded = expanded_dirs.contains(&path);
        rows.push(SidebarRow {
            key: SidebarNodePath::Directory(path.clone()),
            parent_path: parent_path.to_owned(),
            sort_name: directory_name.clone(),
            depth,
            kind: SidebarRowKind::Directory { expanded },
        });

        if expanded {
            append_sidebar_rows(&path, depth + 1, child, expanded_dirs, snapshot, rows);
        }
    }

    for (file_name, file_index) in &tree.files {
        rows.push(SidebarRow {
            key: SidebarNodePath::File(join_sidebar_path(parent_path, file_name)),
            parent_path: parent_path.to_owned(),
            sort_name: file_name.clone(),
            depth,
            kind: SidebarRowKind::File {
                file_index: *file_index,
                stats: sidebar_file_stats(&snapshot.files[*file_index]),
            },
        });
    }
}

fn join_sidebar_path(parent_path: &str, segment: &str) -> String {
    if parent_path.is_empty() {
        segment.to_owned()
    } else {
        format!("{parent_path}/{segment}")
    }
}

fn sidebar_file_stats(file: &ReviewFile) -> SidebarFileStats {
    let mut stats = SidebarFileStats::default();
    for hunk in &file.patch.hunks {
        for line in &hunk.lines {
            match line.kind {
                frame_core::LineKind::Added => stats.added += 1,
                frame_core::LineKind::Removed => stats.removed += 1,
                frame_core::LineKind::Context => {}
            }
        }
    }

    stats
}

fn sidebar_cursor_index_for_file(rows: &[SidebarRow], file_path: &str) -> Option<usize> {
    if let Some(index) = rows
        .iter()
        .position(|row| matches!(&row.key, SidebarNodePath::File(path) if path == file_path))
    {
        return Some(index);
    }

    for parent_path in sidebar_parent_paths(file_path).into_iter().rev() {
        if let Some(index) = rows.iter().position(
            |row| matches!(&row.key, SidebarNodePath::Directory(path) if path == &parent_path),
        ) {
            return Some(index);
        }
    }

    None
}

fn sidebar_file_order(snapshot: &ReviewSnapshot) -> Vec<usize> {
    build_sidebar_rows(snapshot, &sidebar_directory_paths(snapshot))
        .into_iter()
        .filter_map(|row| row.file_index())
        .collect()
}

fn sidebar_restore_index(rows: &[SidebarRow], previous_key: &SidebarNodePath) -> Option<usize> {
    if let Some(index) = rows.iter().position(|row| &row.key == previous_key) {
        return Some(index);
    }

    let path = previous_key.path();
    let (parent_path, sort_name) = path
        .rsplit_once('/')
        .map_or(("", path), |(parent, name)| (parent, name));
    let target_rank = sidebar_key_rank(previous_key);
    let sibling_indexes = rows
        .iter()
        .enumerate()
        .filter(|(_, row)| row.parent_path == parent_path)
        .map(|(index, row)| (index, sidebar_row_rank(row), row.sort_name.as_str()))
        .collect::<Vec<_>>();

    if let Some((index, _, _)) = sibling_indexes
        .iter()
        .copied()
        .find(|(_, rank, name)| (*rank, *name) > (target_rank, sort_name))
    {
        return Some(index);
    }

    if let Some((index, _, _)) = sibling_indexes.last().copied() {
        return Some(index);
    }

    rows.iter().position(|row| row.file_index().is_some())
}

fn sidebar_first_child_index(rows: &[SidebarRow], row_index: usize) -> Option<usize> {
    let row = rows.get(row_index)?;
    row.directory_expanded()?;

    let next_index = row_index + 1;
    let next_row = rows.get(next_index)?;
    (next_row.depth == row.depth + 1 && next_row.parent_path == row.path()).then_some(next_index)
}

fn sidebar_key_rank(key: &SidebarNodePath) -> usize {
    match key {
        SidebarNodePath::Directory(_) => 0,
        SidebarNodePath::File(_) => 1,
    }
}

fn sidebar_row_rank(row: &SidebarRow) -> usize {
    sidebar_key_rank(&row.key)
}

fn sidebar_row_to_text(
    app: &App,
    row: &SidebarRow,
    row_index: usize,
    width: usize,
) -> Line<'static> {
    let is_cursor =
        app.interaction_mode == InteractionMode::Explorer && row_index == app.sidebar_cursor_row;
    let is_active_file = row.file_index() == Some(app.active_file_index);
    let fill_style = sidebar_fill_style(is_active_file, is_cursor);
    let label = sidebar_row_label(row);
    let metadata = sidebar_metadata_spans(app, row, is_active_file, is_cursor);
    let metadata_width = spans_display_width(&metadata);
    let gap_width = usize::from(!metadata.is_empty() && width > metadata_width);
    let label_width = width.saturating_sub(metadata_width + gap_width);
    let mut spans = Vec::new();

    if label_width > 0 {
        let label_style = apply_sidebar_emphasis(
            match &row.kind {
                SidebarRowKind::Directory { .. } => Style::default().fg(Color::Gray),
                SidebarRowKind::File { .. } => Style::default().fg(if is_active_file {
                    Color::Yellow
                } else {
                    Color::Gray
                }),
            },
            is_active_file,
            is_cursor,
        );
        spans.push(Span::styled(
            fit_display_text(&label, label_width),
            label_style,
        ));
    }

    if gap_width > 0 {
        spans.push(Span::styled(" ".repeat(gap_width), fill_style));
    }
    spans.extend(metadata);
    pad_spans_to_display_width(&mut spans, width, fill_style);
    Line::from(spans)
}

fn sidebar_row_label(row: &SidebarRow) -> String {
    match &row.kind {
        SidebarRowKind::Directory { expanded } => format!(
            "{}{}{}/",
            "  ".repeat(row.depth),
            if *expanded { "▾ " } else { "▸ " },
            row.sort_name
        ),
        SidebarRowKind::File { .. } => format!("{}{}", "  ".repeat(row.depth + 1), row.sort_name),
    }
}

fn sidebar_metadata_spans(
    app: &App,
    row: &SidebarRow,
    is_active_file: bool,
    is_cursor: bool,
) -> Vec<Span<'static>> {
    let Some(file_index) = row.file_index() else {
        return Vec::new();
    };

    let file = &app.snapshot.files[file_index];
    let SidebarRowKind::File { stats, .. } = &row.kind else {
        return Vec::new();
    };
    let comment_count = app.comment_count_for_file(file.display_path());
    let mut spans = vec![
        Span::styled(
            format!("[{}]", file.patch.change),
            apply_sidebar_emphasis(
                Style::default().fg(sidebar_status_color(file.patch.change)),
                is_active_file,
                is_cursor,
            ),
        ),
        Span::styled(
            format!(" +{}", stats.added),
            apply_sidebar_emphasis(Style::default().fg(Color::Green), is_active_file, is_cursor),
        ),
        Span::styled(
            format!(" -{}", stats.removed),
            apply_sidebar_emphasis(Style::default().fg(Color::Red), is_active_file, is_cursor),
        ),
    ];

    if comment_count > 0 {
        spans.push(Span::styled(
            format!(" !{comment_count}"),
            apply_sidebar_emphasis(
                Style::default().fg(Color::Yellow),
                is_active_file,
                is_cursor,
            ),
        ));
    }

    spans
}

fn sidebar_status_color(change: frame_core::FileChangeKind) -> Color {
    match change {
        frame_core::FileChangeKind::Added => Color::Green,
        frame_core::FileChangeKind::Copied => Color::Cyan,
        frame_core::FileChangeKind::Deleted => Color::Red,
        frame_core::FileChangeKind::Modified => Color::Yellow,
        frame_core::FileChangeKind::Renamed => Color::Magenta,
    }
}

fn sidebar_fill_style(is_active_file: bool, is_cursor: bool) -> Style {
    let mut style = Style::default();
    if is_active_file {
        style = style.bg(Color::Rgb(28, 28, 20));
    }
    if is_cursor {
        style = style.bg(Color::Rgb(58, 58, 74));
    }
    style
}

fn apply_sidebar_emphasis(mut style: Style, is_active_file: bool, is_cursor: bool) -> Style {
    style = style.patch(sidebar_fill_style(is_active_file, is_cursor));
    if is_active_file {
        style = style.add_modifier(Modifier::BOLD);
    }
    if is_cursor {
        style = style.add_modifier(Modifier::BOLD);
    }
    style
}

fn spans_display_width(spans: &[Span<'_>]) -> usize {
    spans
        .iter()
        .map(|span| UnicodeWidthStr::width(span.content.as_ref()))
        .sum()
}

fn fit_display_text(text: &str, width: usize) -> String {
    if width == 0 {
        return String::new();
    }

    if UnicodeWidthStr::width(text) <= width {
        return pad_display_text(text, width);
    }

    if width == 1 {
        return "…".to_owned();
    }

    let end = take_width_bounded_chunk_end(text, 0, width - 1);
    let mut fitted = text[..end].to_owned();
    fitted.push('…');
    fitted
}

fn pad_display_text(text: &str, width: usize) -> String {
    let padding = width.saturating_sub(UnicodeWidthStr::width(text));
    format!("{text}{}", " ".repeat(padding))
}

fn first_anchor_line(file: &ReviewFile) -> usize {
    file.anchors
        .first()
        .map_or(0, |anchor| anchor.buffer_line)
        .min(file.buffer.line_count().saturating_sub(1))
}

fn line_index_for_file(file: &ReviewFile, line: usize) -> usize {
    line.min(file.buffer.line_count().saturating_sub(1))
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

fn top_aligned_viewport_top(cursor_row: usize, total_rows: usize, height: usize) -> usize {
    if total_rows <= height {
        return 0;
    }

    let max_top = total_rows.saturating_sub(height);
    cursor_row.min(max_top)
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
            .is_some_and(|line| app.line_in_selection(line));
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
            Span::styled(pad_comment_segment(&segment, inner_width), body_style),
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

            if comment_text_width(&current) + 1 + comment_text_width(word) <= width {
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
    if comment_text_width(word) <= width {
        current.push_str(word);
        return;
    }

    let mut start = 0;
    while start < word.len() {
        let end = take_width_bounded_chunk_end(word, start, width);
        let chunk = word[start..end].to_owned();
        if current.is_empty() {
            if end < word.len() {
                wrapped.push(chunk);
            } else {
                current.push_str(&chunk);
            }
        } else {
            wrapped.push(std::mem::take(current));
            if end < word.len() {
                wrapped.push(chunk);
            } else {
                current.push_str(&chunk);
            }
        }
        start = end;
    }
}

fn comment_text_width(text: &str) -> usize {
    UnicodeWidthStr::width(text)
}

fn pad_comment_segment(segment: &str, width: usize) -> String {
    let padding = width.saturating_sub(comment_text_width(segment));
    format!("{segment}{}", " ".repeat(padding))
}

fn take_width_bounded_chunk_end(word: &str, start: usize, width: usize) -> usize {
    let mut end = start;
    let mut used_width = 0;

    for (offset, ch) in word[start..].char_indices() {
        let char_width = UnicodeWidthChar::width(ch).unwrap_or(0);
        if end > start && used_width + char_width > width {
            break;
        }

        used_width += char_width;
        end = start + offset + ch.len_utf8();

        if used_width >= width {
            break;
        }
    }

    if end == start {
        start
            + word[start..]
                .chars()
                .next()
                .expect("slice is non-empty")
                .len_utf8()
    } else {
        end
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

fn raw_hunk_targets_in_rows(rows: &[RawRenderRow]) -> Vec<usize> {
    rows.iter()
        .enumerate()
        .filter_map(|(index, row)| (row.kind == RawRowKind::HunkHeader).then_some(index))
        .collect()
}

#[cfg(test)]
fn raw_row_for_buffer_line(file: &ReviewFile, buffer_line: usize) -> usize {
    let rows = raw_rows(file);
    raw_row_for_buffer_line_in_rows(file, &rows, buffer_line)
}

fn raw_row_for_buffer_line_in_rows(
    file: &ReviewFile,
    rows: &[RawRenderRow],
    buffer_line: usize,
) -> usize {
    let target_lineno = buffer_line + 1;
    rows.iter()
        .position(|row| relevant_raw_lineno(file, row) == Some(target_lineno))
        .or_else(|| {
            rows.iter().position(|row| {
                relevant_raw_lineno(file, row).is_some_and(|lineno| lineno > target_lineno)
            })
        })
        .or_else(|| {
            rows.iter().rposition(|row| {
                relevant_raw_lineno(file, row).is_some_and(|lineno| lineno < target_lineno)
            })
        })
        .unwrap_or(0)
}

fn buffer_line_for_raw_row_in_rows(
    file: &ReviewFile,
    rows: &[RawRenderRow],
    row_index: usize,
) -> Option<usize> {
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

    if let Some(target) = app.selection_target()
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

    overlays
}

fn target_segment_for_line(
    target: &CommentTarget,
    line_index: usize,
    line_text: &str,
) -> Option<(usize, usize)> {
    let CommentTarget::LineRange {
        start_line,
        end_line,
    } = target.normalized();
    (start_line <= line_index && line_index <= end_line && !line_text.is_empty())
        .then_some((0, line_text.len()))
}

fn selection_chunk_style() -> Style {
    Style::default().bg(Color::Rgb(52, 52, 72))
}

fn comment_chunk_style() -> Style {
    Style::default().add_modifier(Modifier::BOLD | Modifier::UNDERLINED)
}

fn format_comment_target(file_path: &str, target: &CommentTarget) -> String {
    let CommentTarget::LineRange {
        start_line,
        end_line,
    } = target.normalized();
    if start_line == end_line {
        format!("{file_path}:{}", start_line + 1)
    } else {
        format!("{file_path}:{}-{}", start_line + 1, end_line + 1)
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

fn pad_spans_to_display_width(spans: &mut Vec<Span<'static>>, width: usize, style: Style) {
    let current_width = spans_display_width(spans);

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
    use std::{
        collections::BTreeSet,
        fs,
        path::{Path, PathBuf},
        process::Command,
        sync::{
            Arc,
            atomic::{AtomicBool, AtomicUsize, Ordering},
            mpsc,
        },
        thread,
        time::{Duration, SystemTime, UNIX_EPOCH},
    };

    use frame_core::{
        BufferSource, FileChangeKind, LineKind, PatchFile, PatchHunk, PatchLine, ReviewFile,
        ReviewFileInput, ReviewSnapshot,
    };
    use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
    use ratatui::style::Color;

    use super::{
        App, AppEvent, CodeRowKind, CommentTarget, InputMode, InteractionMode, MotionMode,
        RawRowKind, RefreshFilter, SidebarFileStats, SidebarNodePath, SidebarRow, SidebarRowKind,
        ViewMode, build_sidebar_rows, code_rows, comment_box_lines, raw_hunk_targets_in_rows,
        raw_row_for_buffer_line, raw_row_to_text, raw_rows, relevant_raw_lineno,
        rendered_code_view, run_refresh_loop, sidebar_directory_paths, sidebar_row_to_text,
        top_aligned_viewport_top,
    };

    fn key(code: KeyCode) -> KeyEvent {
        KeyEvent::new(code, KeyModifiers::NONE)
    }

    static TEMP_COUNTER: AtomicUsize = AtomicUsize::new(0);

    #[derive(Debug)]
    struct TempGitDir {
        path: PathBuf,
    }

    impl TempGitDir {
        fn new(name: &str) -> Self {
            let counter = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
            let unique = format!(
                "frame-view-{name}-{}-{}-{counter}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .expect("system time should be after the unix epoch")
                    .as_nanos()
            );
            let path = std::env::temp_dir().join(unique);
            fs::create_dir_all(&path).expect("temp dir should be created");
            Self { path }
        }

        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempGitDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    fn git(cwd: &Path, args: &[&str]) {
        let status = Command::new("git")
            .current_dir(cwd)
            .args(args)
            .status()
            .expect("git command should start");
        assert!(status.success(), "git command should succeed");
    }

    fn write(path: &Path, contents: &str) {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("parent directories should be created");
        }
        fs::write(path, contents).expect("file write should succeed");
    }

    fn init_git_repo() -> TempGitDir {
        let temp = TempGitDir::new("repo");
        git(temp.path(), &["init", "--quiet"]);
        git(
            temp.path(),
            &["config", "user.email", "frame-tests@example.com"],
        );
        git(temp.path(), &["config", "user.name", "Frame Tests"]);
        temp
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

    fn sample_root_file() -> ReviewFile {
        ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: Some("Cargo.toml".to_owned()),
                new_path: Some("Cargo.toml".to_owned()),
                change: FileChangeKind::Modified,
                hunks: vec![PatchHunk {
                    header: "@@ -1 +1 @@".to_owned(),
                    old_start: 1,
                    old_len: 1,
                    new_start: 1,
                    new_len: 1,
                    lines: vec![
                        PatchLine {
                            kind: LineKind::Removed,
                            old_lineno: Some(1),
                            new_lineno: None,
                            text: "name = \"old\"".to_owned(),
                        },
                        PatchLine {
                            kind: LineKind::Added,
                            old_lineno: None,
                            new_lineno: Some(1),
                            text: "name = \"frame\"".to_owned(),
                        },
                    ],
                }],
                has_binary_or_unrenderable_change: false,
            },
            buffer: frame_core::CodeBuffer::from_text("name = \"frame\"\n"),
            source: BufferSource::PostImage,
        })
    }

    fn sample_nested_view_file() -> ReviewFile {
        ReviewFile::new(ReviewFileInput {
            patch: PatchFile {
                old_path: None,
                new_path: Some("src/ui/view.rs".to_owned()),
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
                        text: "pub fn render() {}".to_owned(),
                    }],
                }],
                has_binary_or_unrenderable_change: false,
            },
            buffer: frame_core::CodeBuffer::from_text("pub fn render() {}\n"),
            source: BufferSource::PostImage,
        })
    }

    fn sample_snapshot() -> ReviewSnapshot {
        ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_main_file(), sample_added_file()],
        }
    }

    fn sample_tree_snapshot() -> ReviewSnapshot {
        ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![
                sample_root_file(),
                sample_main_file(),
                sample_added_file(),
                sample_nested_view_file(),
            ],
        }
    }

    fn sample_main_file_with_tail() -> ReviewFile {
        let sample = sample_main_file();
        ReviewFile::new(ReviewFileInput {
            patch: sample.patch,
            buffer: frame_core::CodeBuffer::from_text(
                "fn main() {\n    new();\n    extra();\n}\n\n\nfn later() {\n    other();\n}\nfn tail() {}\n",
            ),
            source: BufferSource::PostImage,
        })
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
        let mut app = App::new(sample_tree_snapshot());
        app.set_active_file(1);

        assert_eq!(app.code_cursor_line, 1);
        app.jump_next_change(1);
        assert_eq!(app.code_cursor_line, 7);
        app.set_active_file(3);
        app.jump_next_file(1);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/lib.rs")
        );
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
        assert_eq!(app.interaction_mode, InteractionMode::Content);
        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert!(app.file_explorer_open);
        assert_eq!(app.interaction_mode, InteractionMode::Explorer);
        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert!(!app.file_explorer_open);
        assert_eq!(app.interaction_mode, InteractionMode::Content);
    }

    #[test]
    fn app_toggles_visual_mode_with_v() {
        let mut app = App::new(sample_snapshot());

        assert_eq!(app.motion_mode, MotionMode::Normal);
        assert!(!app.handle_key(key(KeyCode::Char('v'))));
        assert_eq!(app.motion_mode, MotionMode::Visual);
        assert_eq!(
            app.selection_target(),
            Some(CommentTarget::LineRange {
                start_line: 1,
                end_line: 1,
            })
        );
        assert!(!app.handle_key(key(KeyCode::Char('j'))));
        assert_eq!(
            app.selection_target(),
            Some(CommentTarget::LineRange {
                start_line: 1,
                end_line: 2,
            })
        );
        assert!(!app.handle_key(key(KeyCode::Esc)));
        assert_eq!(app.motion_mode, MotionMode::Normal);
        assert_eq!(app.selection_target(), None);
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
        assert_eq!(
            app.comments[0].target,
            CommentTarget::LineRange {
                start_line: 1,
                end_line: 2,
            }
        );
        assert_eq!(app.motion_mode, MotionMode::Normal);
    }

    #[test]
    fn visual_selection_normalizes_reversed_line_ranges() {
        let mut app = App::new(sample_snapshot());

        app.set_code_cursor_line(2);
        assert!(!app.handle_key(key(KeyCode::Char('v'))));
        assert!(!app.handle_key(key(KeyCode::Char('k'))));

        assert_eq!(
            app.selection_target(),
            Some(CommentTarget::LineRange {
                start_line: 1,
                end_line: 2,
            })
        );
    }

    #[test]
    fn raw_diff_cursor_stays_near_trailing_unchanged_lines() {
        let sample = sample_main_file();
        let file = ReviewFile::new(ReviewFileInput {
            patch: sample.patch,
            buffer: frame_core::CodeBuffer::from_text(
                "fn main() {\n    new();\n    extra();\n}\n\n\nfn later() {\n    other();\n}\nfn tail() {}\n",
            ),
            source: BufferSource::PostImage,
        });

        let row_index = raw_row_for_buffer_line(&file, 9);
        let rows = raw_rows(&file);

        assert_eq!(relevant_raw_lineno(&file, &rows[row_index]), Some(9));
    }

    #[test]
    fn code_view_h_l_zero_caret_and_dollar_are_noops() {
        let mut app = App::new(sample_snapshot());

        assert!(!app.handle_key(key(KeyCode::Char('h'))));
        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert!(!app.handle_key(key(KeyCode::Char('2'))));
        assert!(!app.handle_key(key(KeyCode::Char('l'))));
        assert!(!app.handle_key(key(KeyCode::Char('0'))));
        assert!(!app.handle_key(key(KeyCode::Char('^'))));
        assert!(!app.handle_key(key(KeyCode::Char('$'))));
        assert_eq!(app.code_cursor_line, 1);
    }

    #[test]
    fn zt_aligns_cursor_to_top_in_code_view() {
        let mut app = App::new(sample_snapshot());
        app.set_viewport_size(4, 80);
        app.set_code_cursor_line(7);

        let file = app.active_file().expect("file exists");
        let rendered = rendered_code_view(&app, file, app.viewport_width);
        let expected_top = top_aligned_viewport_top(
            rendered.cursor_visual_row,
            rendered.lines.len(),
            app.viewport_height,
        );

        assert!(!app.handle_key(key(KeyCode::Char('z'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert_eq!(app.code_viewport_top, expected_top);
    }

    #[test]
    fn zt_aligns_cursor_to_top_in_raw_diff_view() {
        let mut app = App::new(sample_snapshot());
        app.set_viewport_size(4, 80);
        app.toggle_mode();
        app.raw_cursor_line = 4;

        let rows = app.active_raw_rows().expect("raw rows exist");
        let expected_top =
            top_aligned_viewport_top(app.raw_cursor_line, rows.len(), app.viewport_height);

        assert!(!app.handle_key(key(KeyCode::Char('z'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert_eq!(app.raw_viewport_top, expected_top);
    }

    #[test]
    fn zt_keeps_short_buffers_at_top() {
        let mut app = App::new(sample_snapshot());
        app.set_viewport_size(40, 80);
        app.set_code_cursor_line(7);

        assert!(!app.handle_key(key(KeyCode::Char('z'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert_eq!(app.code_viewport_top, 0);
    }

    #[test]
    fn bare_t_clears_pending_count_before_next_motion() {
        let mut app = App::new(sample_snapshot());

        assert!(!app.handle_key(key(KeyCode::Char('2'))));
        assert!(!app.handle_key(key(KeyCode::Char('t'))));
        assert!(!app.handle_key(key(KeyCode::Char('j'))));
        assert_eq!(app.code_cursor_line, 2);
    }

    #[test]
    fn raw_diff_bracket_h_navigation_still_moves_between_hunks() {
        let mut app = App::new(sample_snapshot());
        let file = app.active_file().expect("file exists");
        let rows = raw_rows(file);
        let targets = raw_hunk_targets_in_rows(&rows);
        let expected_start = raw_row_for_buffer_line(file, app.code_cursor_line);
        let expected_next = *targets
            .iter()
            .find(|&&target| target > expected_start)
            .expect("expected a later hunk target");

        app.toggle_mode();
        assert_eq!(app.raw_cursor_line, expected_start);
        assert!(!app.handle_key(key(KeyCode::Char(']'))));
        assert!(!app.handle_key(key(KeyCode::Char('h'))));
        assert_eq!(app.raw_cursor_line, expected_next);
    }

    #[test]
    fn footer_does_not_advertise_chunk_navigation() {
        let app = App::new(sample_snapshot());

        assert!(!app.footer_text().contains("chunks"));
    }

    #[test]
    fn refresh_preserves_active_file_by_path_across_reorder() {
        let mut app = App::new(sample_snapshot());
        app.set_active_file(1);

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_added_file(), sample_main_file()],
        });

        assert_eq!(app.active_file_index, 0);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/lib.rs")
        );
    }

    #[test]
    fn refresh_falls_back_when_active_file_disappears() {
        let mut app = App::new(sample_snapshot());
        app.set_active_file(1);

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_main_file()],
        });

        assert_eq!(app.active_file_index, 0);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/main.rs")
        );
    }

    #[test]
    fn refresh_clears_comment_state_on_snapshot_change() {
        let mut app = App::new(sample_snapshot());
        app.motion_mode = MotionMode::Visual;
        app.visual_anchor = Some(app.cursor_anchor());
        app.input_mode = InputMode::Comment("draft".to_owned());
        app.comments.push(super::ReviewComment {
            file_path: "src/main.rs".to_owned(),
            target: CommentTarget::LineRange {
                start_line: 1,
                end_line: 1,
            },
            text: "Keep this visible".to_owned(),
        });

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_added_file(), sample_main_file()],
        });

        assert!(matches!(app.input_mode, InputMode::Normal));
        assert!(app.comments.is_empty());
        assert_eq!(app.motion_mode, MotionMode::Normal);
    }

    #[test]
    fn refresh_keeps_command_mode_active() {
        let mut app = App::new(sample_snapshot());
        app.input_mode = InputMode::Command("diff".to_owned());

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_added_file(), sample_main_file()],
        });

        assert!(matches!(app.input_mode, InputMode::Command(_)));
    }

    #[test]
    fn refresh_is_noop_for_unchanged_snapshot() {
        let mut app = App::new(sample_snapshot());
        app.comments.push(super::ReviewComment {
            file_path: "src/main.rs".to_owned(),
            target: CommentTarget::LineRange {
                start_line: 1,
                end_line: 1,
            },
            text: "Keep this visible".to_owned(),
        });

        app.apply_snapshot_refresh(app.snapshot.clone());

        assert_eq!(app.comments.len(), 1);
    }

    #[test]
    fn refresh_keeps_raw_diff_cursor_near_same_buffer_line() {
        let mut app = App::new(sample_snapshot());
        app.code_cursor_line = 7;
        app.toggle_mode();

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![sample_main_file_with_tail(), sample_added_file()],
        });

        let rows = app.active_raw_rows().expect("raw rows should exist");
        assert_eq!(app.view_mode, ViewMode::RawDiff);
        assert_eq!(app.code_cursor_line, 7);
        assert_eq!(
            relevant_raw_lineno(
                app.active_file().expect("active file should exist"),
                &rows[app.raw_cursor_line]
            ),
            Some(8)
        );
    }

    #[test]
    fn footer_keeps_auto_refresh_warning_visible_after_status_changes() {
        let mut app = App::new(sample_snapshot());
        app.set_auto_refresh_warning("Auto-refresh unavailable: watcher failed".to_owned());
        app.set_status("Switched to code view.");

        let footer = app.footer_text();

        assert!(footer.contains("Auto-refresh unavailable: watcher failed"));
        assert!(footer.contains("Switched to code view."));
    }

    #[test]
    fn refresh_filter_uses_gitignore_and_git_metadata_rules() {
        let repo = init_git_repo();
        write(&repo.path().join(".gitignore"), "target/\n");
        write(&repo.path().join("src/main.rs"), "fn main() {}\n");
        write(
            &repo.path().join("target/generated.rs"),
            "pub fn ignored() {}\n",
        );

        let filter = RefreshFilter::new(
            repo.path().to_path_buf(),
            repo.path().join(".git"),
            repo.path().join(".git"),
        );

        assert!(
            filter
                .should_refresh_path(&repo.path().join("src/main.rs"))
                .expect("tracked worktree file should be checked")
        );
        assert!(
            !filter
                .should_refresh_path(&repo.path().join("target/generated.rs"))
                .expect("ignored path should be checked")
        );
        assert!(
            filter
                .should_refresh_path(&repo.path().join(".git/index"))
                .expect("git index should be checked")
        );
        assert!(
            filter
                .should_refresh_path(&repo.path().join(".git/packed-refs"))
                .expect("packed refs should be checked")
        );
        assert!(
            filter
                .should_refresh_path(&repo.path().join(".git/info/exclude"))
                .expect("git info exclude should be checked")
        );
        assert!(
            !filter
                .should_refresh_path(&repo.path().join(".git/objects/ab/cdef"))
                .expect("git object noise should be ignored")
        );
    }

    #[test]
    fn refresh_filter_detects_common_dir_refs_for_worktrees() {
        let repo = init_git_repo();
        let filter = RefreshFilter::new(
            repo.path().to_path_buf(),
            repo.path().join(".git/worktrees/frame"),
            repo.path().join(".git"),
        );

        assert!(
            filter
                .should_refresh_path(&repo.path().join(".git/worktrees/frame/HEAD"))
                .expect("worktree head should be checked")
        );
        assert!(
            filter
                .should_refresh_path(&repo.path().join(".git/refs/heads/main"))
                .expect("common dir refs should be checked")
        );
        assert!(
            !filter
                .should_refresh_path(&repo.path().join(".git/logs/HEAD"))
                .expect("git logs should be ignored")
        );
    }

    #[test]
    fn ignored_events_do_not_enter_refresh_queue() {
        let repo = init_git_repo();
        write(&repo.path().join(".gitignore"), "target/\n");
        write(
            &repo.path().join("target/generated.rs"),
            "pub fn ignored() {}\n",
        );
        let filter = RefreshFilter::new(
            repo.path().to_path_buf(),
            repo.path().join(".git"),
            repo.path().join(".git"),
        );
        let mut pending_paths = BTreeSet::new();
        let mut needs_rescan = false;

        filter
            .queue_relevant_paths(
                &notify::Event {
                    kind: notify::EventKind::Modify(notify::event::ModifyKind::Any),
                    paths: vec![repo.path().join("target/generated.rs")],
                    attrs: notify::event::EventAttributes::new(),
                },
                &mut pending_paths,
                &mut needs_rescan,
            )
            .expect("ignored event should be classified");

        assert!(pending_paths.is_empty());
        assert!(!needs_rescan);
    }

    #[test]
    fn mixed_events_only_queue_unignored_worktree_paths() {
        let repo = init_git_repo();
        write(&repo.path().join(".gitignore"), "target/\n");
        write(&repo.path().join("src/main.rs"), "fn main() {}\n");
        write(
            &repo.path().join("target/generated.rs"),
            "pub fn ignored() {}\n",
        );
        let filter = RefreshFilter::new(
            repo.path().to_path_buf(),
            repo.path().join(".git"),
            repo.path().join(".git"),
        );
        let mut pending_paths = BTreeSet::new();
        let mut needs_rescan = false;

        let queued = filter
            .queue_relevant_paths(
                &notify::Event {
                    kind: notify::EventKind::Modify(notify::event::ModifyKind::Any),
                    paths: vec![
                        repo.path().join("src/main.rs"),
                        repo.path().join("target/generated.rs"),
                    ],
                    attrs: notify::event::EventAttributes::new(),
                },
                &mut pending_paths,
                &mut needs_rescan,
            )
            .expect("mixed event should be classified");

        assert!(queued);
        assert_eq!(pending_paths.len(), 1);
        assert!(pending_paths.contains(&repo.path().join("src/main.rs")));
        assert!(!needs_rescan);
    }

    #[test]
    fn refresh_loop_flushes_pending_refresh_despite_ignored_noise() {
        let repo = init_git_repo();
        write(&repo.path().join(".gitignore"), "target/\n");
        write(&repo.path().join("tracked.txt"), "line one\nline two\n");
        write(
            &repo.path().join("target/generated.rs"),
            "pub fn ignored() {}\n",
        );

        let filter = RefreshFilter::new(
            repo.path().to_path_buf(),
            repo.path().join(".git"),
            repo.path().join(".git"),
        );
        let (watch_tx, watch_rx) = mpsc::channel();
        let (app_event_tx, app_event_rx) = mpsc::channel();
        let (dummy_watch_tx, _dummy_watch_rx) = mpsc::channel();
        let watcher =
            notify::recommended_watcher(dummy_watch_tx).expect("dummy watcher should be created");
        let shutdown = Arc::new(AtomicBool::new(false));
        let refresh_thread = {
            let repo_root = repo.path().to_path_buf();
            let shutdown = Arc::clone(&shutdown);
            thread::spawn(move || {
                run_refresh_loop(
                    &repo_root,
                    &filter,
                    &watch_rx,
                    &app_event_tx,
                    shutdown.as_ref(),
                    watcher,
                );
            })
        };

        watch_tx
            .send(Ok(notify::Event {
                kind: notify::EventKind::Modify(notify::event::ModifyKind::Any),
                paths: vec![repo.path().join("tracked.txt")],
                attrs: notify::event::EventAttributes::new(),
            }))
            .expect("tracked file event should send");

        let noise_repo_root = repo.path().to_path_buf();
        let noise_thread = thread::spawn(move || {
            let start = std::time::Instant::now();
            while start.elapsed() < Duration::from_millis(500) {
                if watch_tx
                    .send(Ok(notify::Event {
                        kind: notify::EventKind::Modify(notify::event::ModifyKind::Any),
                        paths: vec![noise_repo_root.join("target/generated.rs")],
                        attrs: notify::event::EventAttributes::new(),
                    }))
                    .is_err()
                {
                    break;
                }
                thread::sleep(Duration::from_millis(10));
            }
        });

        let refresh_result = app_event_rx
            .recv_timeout(Duration::from_millis(300))
            .expect("refresh should not wait for ignored noise to stop");

        assert!(matches!(refresh_result, AppEvent::RefreshResult(Ok(_))));
        let AppEvent::RefreshResult(Ok(snapshot)) = refresh_result else {
            return;
        };
        let paths: Vec<_> = snapshot
            .files
            .iter()
            .filter_map(|file| file.patch.new_path.as_deref())
            .collect();

        assert!(paths.contains(&"tracked.txt"));
        assert!(!paths.contains(&"target/generated.rs"));

        shutdown.store(true, Ordering::Relaxed);
        noise_thread.join().expect("noise thread should join");
        refresh_thread.join().expect("refresh thread should join");
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
    fn comment_box_wraps_wide_glyphs_by_display_width() {
        use unicode_width::UnicodeWidthStr;

        let lines = comment_box_lines("界界界界界", 10, true);
        let top_width = UnicodeWidthStr::width(lines[0].to_string().as_str());

        assert!(
            lines
                .iter()
                .all(|line| UnicodeWidthStr::width(line.to_string().as_str()) == top_width)
        );
        assert_eq!(lines.len(), 3);
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

    #[test]
    fn sidebar_builds_directory_tree_in_sorted_order() {
        let snapshot = sample_tree_snapshot();
        let rows = build_sidebar_rows(&snapshot, &sidebar_directory_paths(&snapshot));
        let keys = rows.into_iter().map(|row| row.key).collect::<Vec<_>>();

        assert_eq!(
            keys,
            vec![
                SidebarNodePath::Directory("src".to_owned()),
                SidebarNodePath::Directory("src/ui".to_owned()),
                SidebarNodePath::File("src/ui/view.rs".to_owned()),
                SidebarNodePath::File("src/lib.rs".to_owned()),
                SidebarNodePath::File("src/main.rs".to_owned()),
                SidebarNodePath::File("Cargo.toml".to_owned()),
            ]
        );
    }

    #[test]
    fn explorer_mode_live_previews_only_file_rows() {
        let mut app = App::new(sample_tree_snapshot());
        app.enter_explorer_mode();
        app.move_sidebar_to_start(None);

        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src".to_owned()))
        );
        assert_eq!(app.active_file_index, 0);

        app.move_sidebar_down(1);
        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src/ui".to_owned()))
        );
        assert_eq!(app.active_file_index, 0);

        app.move_sidebar_down(1);
        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::File("src/ui/view.rs".to_owned()))
        );
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/ui/view.rs")
        );
    }

    #[test]
    fn escape_leaves_explorer_visible_but_returns_to_content() {
        let mut app = App::new(sample_snapshot());

        assert!(!app.handle_key(key(KeyCode::Char('e'))));
        assert_eq!(app.interaction_mode, InteractionMode::Explorer);
        assert!(app.file_explorer_open);

        assert!(!app.handle_key(key(KeyCode::Esc)));
        assert_eq!(app.interaction_mode, InteractionMode::Content);
        assert!(app.file_explorer_open);
    }

    #[test]
    fn refresh_preserves_sidebar_cursor_and_collapsed_directories() {
        let mut app = App::new(sample_tree_snapshot());
        app.enter_explorer_mode();
        app.move_sidebar_to_start(None);
        app.move_sidebar_down(1);
        let _ = app.collapse_sidebar_directory("src/ui");

        app.apply_snapshot_refresh(ReviewSnapshot {
            repo_root: PathBuf::from("/tmp/frame-test"),
            files: vec![
                sample_nested_view_file(),
                sample_added_file(),
                sample_root_file(),
                sample_main_file(),
            ],
        });

        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src/ui".to_owned()))
        );
        assert!(!app.expanded_dirs.contains("src/ui"));
    }

    #[test]
    fn syncing_active_file_preserves_collapsed_ancestor_directory() {
        let mut app = App::new(sample_tree_snapshot());
        app.set_active_file(1);
        app.enter_explorer_mode();
        app.move_sidebar_to_start(None);
        app.handle_sidebar_enter();

        assert!(!app.expanded_dirs.contains("src"));
        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src".to_owned()))
        );

        app.exit_explorer_mode();
        assert!(!app.expanded_dirs.contains("src"));
        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src".to_owned()))
        );

        app.enter_explorer_mode();
        assert!(!app.expanded_dirs.contains("src"));
        assert_eq!(
            app.current_sidebar_key(),
            Some(SidebarNodePath::Directory("src".to_owned()))
        );
    }

    #[test]
    fn file_jumps_follow_sidebar_tree_order() {
        let mut app = App::new(sample_tree_snapshot());
        app.set_active_file(3);

        app.jump_next_file(1);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/lib.rs")
        );

        app.jump_next_file(1);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/main.rs")
        );

        app.jump_next_file(1);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("Cargo.toml")
        );

        app.jump_previous_file(2);
        assert_eq!(
            app.active_file().map(ReviewFile::display_path),
            Some("src/lib.rs")
        );
    }

    #[test]
    fn sidebar_row_rendering_preserves_metadata_when_label_truncates() {
        use unicode_width::UnicodeWidthStr;

        let app = App::new(sample_snapshot());
        let row = SidebarRow {
            key: SidebarNodePath::File("src/really-long-file-name.rs".to_owned()),
            parent_path: "src".to_owned(),
            sort_name: "really-long-file-name.rs".to_owned(),
            depth: 1,
            kind: SidebarRowKind::File {
                file_index: 0,
                stats: SidebarFileStats {
                    added: 12,
                    removed: 4,
                },
            },
        };

        let line = sidebar_row_to_text(&app, &row, 0, 20).to_string();

        assert!(line.contains('…'));
        assert!(line.contains("[M] +12 -4"));
        assert_eq!(UnicodeWidthStr::width(line.as_str()), 20);
    }
}
