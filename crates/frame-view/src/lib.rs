use std::{collections::BTreeMap, fmt::Write as _, io, time::Duration};

use frame_core::{BufferSource, ChangeKind, ReviewFile, ReviewSnapshot};
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
    text::Line,
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
struct ReviewComment {
    file_path: String,
    buffer_line: usize,
    text: String,
}

#[derive(Debug)]
struct App {
    snapshot: ReviewSnapshot,
    active_file_index: usize,
    file_explorer_open: bool,
    code_cursor_line: usize,
    code_viewport_top: usize,
    raw_cursor_line: usize,
    raw_viewport_top: usize,
    viewport_height: usize,
    pending_sequence: PendingSequence,
    view_mode: ViewMode,
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
            code_viewport_top: 0,
            raw_cursor_line: 0,
            raw_viewport_top: 0,
            viewport_height: 1,
            pending_sequence: PendingSequence::None,
            view_mode: ViewMode::Code,
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
            self.pending_sequence = PendingSequence::None;

            match key.code {
                KeyCode::Char('d') => self.move_half_page_down(),
                KeyCode::Char('u') => self.move_half_page_up(),
                _ => {}
            }

            return false;
        }

        match key.code {
            KeyCode::Char('q') => return true,
            KeyCode::Char(':') => {
                self.pending_sequence = PendingSequence::None;
                self.input_mode = InputMode::Command(String::new());
            }
            KeyCode::Char('i') => {
                self.pending_sequence = PendingSequence::None;
                if self.active_file().is_some() {
                    self.input_mode = InputMode::Comment(String::new());
                }
            }
            KeyCode::Char('e') => {
                self.pending_sequence = PendingSequence::None;
                self.toggle_file_explorer();
            }
            KeyCode::Tab => {
                self.pending_sequence = PendingSequence::None;
                self.toggle_mode();
            }
            KeyCode::Char('j') => {
                self.pending_sequence = PendingSequence::None;
                self.move_down();
            }
            KeyCode::Char('k') => {
                self.pending_sequence = PendingSequence::None;
                self.move_up();
            }
            KeyCode::Char('G') => {
                self.pending_sequence = PendingSequence::None;
                self.move_to_end();
            }
            KeyCode::Char('g') => {
                if self.pending_sequence == PendingSequence::G {
                    self.move_to_start();
                    self.pending_sequence = PendingSequence::None;
                } else {
                    self.pending_sequence = PendingSequence::G;
                }
            }
            KeyCode::Char('d') => {
                if self.pending_sequence == PendingSequence::G {
                    self.toggle_mode();
                }
                self.pending_sequence = PendingSequence::None;
            }
            KeyCode::Char(']') => {
                self.pending_sequence = PendingSequence::CloseBracket;
            }
            KeyCode::Char('[') => {
                self.pending_sequence = PendingSequence::OpenBracket;
            }
            KeyCode::Char('c') => {
                if self.pending_sequence == PendingSequence::CloseBracket {
                    self.jump_next_change();
                } else if self.pending_sequence == PendingSequence::OpenBracket {
                    self.jump_previous_change();
                }
                self.pending_sequence = PendingSequence::None;
            }
            KeyCode::Char('f') => {
                if self.pending_sequence == PendingSequence::CloseBracket {
                    self.jump_next_file();
                } else if self.pending_sequence == PendingSequence::OpenBracket {
                    self.jump_previous_file();
                }
                self.pending_sequence = PendingSequence::None;
            }
            KeyCode::Char('h') => {
                if self.pending_sequence == PendingSequence::CloseBracket {
                    self.jump_next_hunk();
                } else if self.pending_sequence == PendingSequence::OpenBracket {
                    self.jump_previous_hunk();
                }
                self.pending_sequence = PendingSequence::None;
            }
            _ => {
                self.pending_sequence = PendingSequence::None;
            }
        }

        false
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

        let Some(file) = self.active_file() else {
            self.set_status("No active file for comment.");
            return;
        };

        let line = self
            .code_cursor_line
            .min(file.buffer.line_count().saturating_sub(1));
        let file_path = file.display_path().to_owned();
        let display_line = line + 1;

        self.comments.push(ReviewComment {
            file_path: file_path.clone(),
            buffer_line: line,
            text: comment,
        });
        self.status_message =
            format!("Queued AI comment on {file_path}:{display_line}. Use :comments to review.");
    }

    fn move_up(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                self.code_cursor_line = self.code_cursor_line.saturating_sub(1);
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = self.raw_cursor_line.saturating_sub(1);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_down(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| file.buffer.line_count().saturating_sub(1));
                self.code_cursor_line = (self.code_cursor_line + 1).min(max_index);
            }
            ViewMode::RawDiff => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| raw_rows(file).len().saturating_sub(1));
                self.raw_cursor_line = (self.raw_cursor_line + 1).min(max_index);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_to_start(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                self.code_cursor_line = 0;
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = 0;
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_to_end(&mut self) {
        match self.view_mode {
            ViewMode::Code => {
                self.code_cursor_line = self
                    .active_file()
                    .map_or(0, |file| file.buffer.line_count().saturating_sub(1));
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = self
                    .active_file()
                    .map_or(0, |file| raw_rows(file).len().saturating_sub(1));
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn move_half_page_down(&mut self) {
        let step = self.half_page_step();

        match self.view_mode {
            ViewMode::Code => {
                let max_index = self
                    .active_file()
                    .map_or(0, |file| file.buffer.line_count().saturating_sub(1));
                self.code_cursor_line = (self.code_cursor_line + step).min(max_index);
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

    fn move_half_page_up(&mut self) {
        let step = self.half_page_step();

        match self.view_mode {
            ViewMode::Code => {
                self.code_cursor_line = self.code_cursor_line.saturating_sub(step);
            }
            ViewMode::RawDiff => {
                self.raw_cursor_line = self.raw_cursor_line.saturating_sub(step);
                self.sync_code_cursor_from_raw();
            }
        }
    }

    fn jump_next_change(&mut self) {
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
                if let Some(target) = next_target(&targets, self.code_cursor_line) {
                    self.code_cursor_line = target;
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
                if let Some(target) = next_target(&targets, self.raw_cursor_line) {
                    self.raw_cursor_line = target;
                    self.sync_code_cursor_from_raw();
                }
            }
        }
    }

    fn jump_previous_change(&mut self) {
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
                if let Some(target) = previous_target(&targets, self.code_cursor_line) {
                    self.code_cursor_line = target;
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
                if let Some(target) = previous_target(&targets, self.raw_cursor_line) {
                    self.raw_cursor_line = target;
                    self.sync_code_cursor_from_raw();
                }
            }
        }
    }

    fn jump_next_file(&mut self) {
        if self.snapshot.files.is_empty() {
            return;
        }

        let targets = (0..self.snapshot.files.len()).collect::<Vec<_>>();
        if let Some(target) = next_target(&targets, self.active_file_index) {
            self.set_active_file(target);
        }
    }

    fn jump_previous_file(&mut self) {
        if self.snapshot.files.is_empty() {
            return;
        }

        let targets = (0..self.snapshot.files.len()).collect::<Vec<_>>();
        if let Some(target) = previous_target(&targets, self.active_file_index) {
            self.set_active_file(target);
        }
    }

    fn jump_next_hunk(&mut self) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(file) = self.active_file() else {
            return;
        };
        let targets = raw_hunk_targets(file);
        if let Some(target) = next_target(&targets, self.raw_cursor_line) {
            self.raw_cursor_line = target;
            self.sync_code_cursor_from_raw();
        }
    }

    fn jump_previous_hunk(&mut self) {
        if self.view_mode != ViewMode::RawDiff {
            return;
        }

        let Some(file) = self.active_file() else {
            return;
        };
        let targets = raw_hunk_targets(file);
        if let Some(target) = previous_target(&targets, self.raw_cursor_line) {
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
                self.view_mode = ViewMode::RawDiff;
                self.set_status("Switched to raw diff view.");
            }
            ViewMode::RawDiff => {
                self.view_mode = ViewMode::Code;
                self.set_status("Switched to code view.");
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
        self.reset_active_file_positions();
    }

    fn reset_active_file_positions(&mut self) {
        let (code_cursor, raw_cursor) = if let Some(file) = self.active_file() {
            let code_cursor = first_anchor_line(file);
            let raw_cursor = raw_row_for_buffer_line(file, code_cursor);
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
            .and_then(|file| buffer_line_for_raw_row(file, self.raw_cursor_line))
            .unwrap_or(self.code_cursor_line);
        self.code_cursor_line = new_cursor;
    }

    fn sync_viewport(&mut self, height: usize) {
        self.viewport_height = height.max(1);

        match self.view_mode {
            ViewMode::Code => self.sync_code_viewport(),
            ViewMode::RawDiff => self.sync_raw_viewport(),
        }
    }

    fn sync_code_viewport(&mut self) {
        let Some(file) = self.active_file() else {
            self.code_viewport_top = 0;
            return;
        };
        let rows = code_rows(file);
        let cursor_row = code_cursor_visual_row(&rows, self.code_cursor_line);
        self.code_viewport_top = sync_viewport_top(
            self.code_viewport_top,
            cursor_row,
            rows.len(),
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
        self.comments
            .iter()
            .any(|comment| comment.file_path == file_path && comment.buffer_line == line_index)
    }

    fn footer_text(&self) -> String {
        match &self.input_mode {
            InputMode::Normal => format!(
                "{} | {} queued | e explorer | : commands | i comment | gd/tab toggle | [c/]c change | [f/]f file",
                self.status_message,
                self.comments.len()
            ),
            InputMode::Command(buffer) => format!(":{buffer}"),
            InputMode::Comment(buffer) => format!("comment> {buffer}"),
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
    app.sync_viewport(content_height.max(1));

    let content = match app.active_file() {
        Some(file) => match app.view_mode {
            ViewMode::Code => {
                let rows = code_rows(file);
                rows.iter()
                    .skip(app.code_viewport_top)
                    .take(content_height.max(1))
                    .map(|row| {
                        let is_selected = row.buffer_line == Some(app.code_cursor_line);
                        let has_comment = row
                            .buffer_line
                            .is_some_and(|line| app.line_has_comment(file.display_path(), line));
                        code_row_to_text(is_selected, has_comment, row)
                    })
                    .collect::<Vec<_>>()
            }
            ViewMode::RawDiff => {
                let rows = raw_rows(file);
                rows.iter()
                    .enumerate()
                    .skip(app.raw_viewport_top)
                    .take(content_height.max(1))
                    .map(|(index, row)| raw_row_to_text(index == app.raw_cursor_line, row))
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

fn next_target(targets: &[usize], current: usize) -> Option<usize> {
    targets
        .iter()
        .copied()
        .find(|target| *target > current)
        .or_else(|| targets.last().copied())
}

fn previous_target(targets: &[usize], current: usize) -> Option<usize> {
    targets
        .iter()
        .copied()
        .take_while(|target| *target < current)
        .last()
        .or_else(|| targets.first().copied())
}

fn code_row_to_text(is_selected: bool, has_comment: bool, row: &CodeRenderRow) -> Line<'static> {
    let base_style = match row.kind {
        CodeRowKind::Buffer => match row.change {
            Some(ChangeKind::Added) => Style::default().fg(Color::Green),
            Some(ChangeKind::Modified) => Style::default().fg(Color::Yellow),
            Some(ChangeKind::Deleted) => Style::default().fg(Color::Red),
            None => Style::default().fg(Color::Gray),
        },
        CodeRowKind::VirtualDeleted => Style::default()
            .fg(Color::Red)
            .add_modifier(Modifier::ITALIC),
        CodeRowKind::Banner => Style::default()
            .fg(Color::Magenta)
            .add_modifier(Modifier::ITALIC),
    };

    let style = match (is_selected, has_comment) {
        (true, true) => base_style
            .add_modifier(Modifier::REVERSED)
            .add_modifier(Modifier::BOLD),
        (true, false) => base_style.add_modifier(Modifier::REVERSED),
        (false, true) => base_style.add_modifier(Modifier::BOLD),
        (false, false) => base_style,
    };

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
    let text = format!(
        "{:>4} {}{} {}",
        format_lineno(row.lineno),
        change_marker,
        comment_marker,
        row.text
    );

    Line::styled(text, style)
}

fn raw_row_to_text(is_selected: bool, row: &RawRenderRow) -> Line<'static> {
    let base_style = match row.kind {
        RawRowKind::HunkHeader => Style::default().fg(Color::Blue),
        RawRowKind::Added => Style::default().fg(Color::Green),
        RawRowKind::Removed => Style::default().fg(Color::Red),
        RawRowKind::Context => Style::default().fg(Color::DarkGray),
        RawRowKind::Placeholder => Style::default()
            .fg(Color::Magenta)
            .add_modifier(Modifier::ITALIC),
    };
    let style = if is_selected {
        base_style.add_modifier(Modifier::REVERSED)
    } else {
        base_style
    };

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

    Line::styled(text, style)
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

    use super::{App, CodeRowKind, InputMode, ViewMode, code_rows};

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
        app.jump_next_change();
        assert_eq!(app.code_cursor_line, 7);
        app.jump_next_file();
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
}
