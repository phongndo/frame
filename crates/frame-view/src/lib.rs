use std::{io, time::Duration};

use libframe::{Diff, DiffFile, DiffLine, Hunk, LineKind};
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
    widgets::{Block, Borders, List, ListItem, ListState, Paragraph},
};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ViewError {
    #[error("failed to interact with the terminal: {0}")]
    Io(#[from] io::Error),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RenderLineKind {
    FileHeader,
    HunkHeader,
    Added,
    Removed,
    Context,
    Placeholder,
    EmptyState,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct RenderLine {
    file_index: Option<usize>,
    kind: RenderLineKind,
    old_lineno: Option<usize>,
    new_lineno: Option<usize>,
    text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PendingSequence {
    None,
    G,
    OpenBracket,
    CloseBracket,
}

#[derive(Debug)]
struct App {
    diff: Diff,
    rendered_lines: Vec<RenderLine>,
    file_targets: Vec<usize>,
    hunk_targets: Vec<usize>,
    cursor_line: usize,
    viewport_top: usize,
    viewport_height: usize,
    pending_sequence: PendingSequence,
}

impl App {
    fn new(diff: Diff) -> Self {
        let mut rendered_lines = Vec::new();
        let mut file_targets = Vec::new();
        let mut hunk_targets = Vec::new();

        if diff.is_empty() {
            rendered_lines.push(RenderLine {
                file_index: None,
                kind: RenderLineKind::EmptyState,
                old_lineno: None,
                new_lineno: None,
                text: "No changes in the current repository.".to_owned(),
            });
        } else {
            for (file_index, file) in diff.files.iter().enumerate() {
                file_targets.push(rendered_lines.len());
                rendered_lines.push(file_header_line(file_index, file));

                if file.has_binary_or_unrenderable_change && file.hunks.is_empty() {
                    rendered_lines.push(RenderLine {
                        file_index: Some(file_index),
                        kind: RenderLineKind::Placeholder,
                        old_lineno: None,
                        new_lineno: None,
                        text: "[binary or unrenderable diff]".to_owned(),
                    });
                }

                for hunk in &file.hunks {
                    hunk_targets.push(rendered_lines.len());
                    rendered_lines.push(hunk_header_line(file_index, hunk));
                    rendered_lines.extend(
                        hunk.lines
                            .iter()
                            .map(|line| diff_content_line(file_index, line)),
                    );
                }
            }
        }

        Self {
            diff,
            rendered_lines,
            file_targets,
            hunk_targets,
            cursor_line: 0,
            viewport_top: 0,
            viewport_height: 1,
            pending_sequence: PendingSequence::None,
        }
    }

    fn handle_key(&mut self, key: KeyEvent) -> bool {
        let should_process = matches!(key.kind, KeyEventKind::Press | KeyEventKind::Repeat);
        if !should_process {
            return false;
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
            KeyCode::Char(']') => {
                self.pending_sequence = PendingSequence::CloseBracket;
            }
            KeyCode::Char('[') => {
                self.pending_sequence = PendingSequence::OpenBracket;
            }
            KeyCode::Char('h') => {
                if self.pending_sequence == PendingSequence::CloseBracket {
                    self.jump_next_hunk();
                } else if self.pending_sequence == PendingSequence::OpenBracket {
                    self.jump_previous_hunk();
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
            _ => {
                self.pending_sequence = PendingSequence::None;
            }
        }

        false
    }

    fn move_up(&mut self) {
        self.cursor_line = self.cursor_line.saturating_sub(1);
    }

    fn move_down(&mut self) {
        let max_index = self.rendered_lines.len().saturating_sub(1);
        self.cursor_line = (self.cursor_line + 1).min(max_index);
    }

    fn move_to_start(&mut self) {
        self.cursor_line = 0;
    }

    fn move_to_end(&mut self) {
        self.cursor_line = self.rendered_lines.len().saturating_sub(1);
    }

    fn move_half_page_down(&mut self) {
        let step = self.half_page_step();
        let max_index = self.rendered_lines.len().saturating_sub(1);
        let max_top = self
            .rendered_lines
            .len()
            .saturating_sub(self.viewport_height.max(1));

        self.cursor_line = (self.cursor_line + step).min(max_index);
        self.viewport_top = (self.viewport_top + step).min(max_top);
    }

    fn move_half_page_up(&mut self) {
        let step = self.half_page_step();
        self.cursor_line = self.cursor_line.saturating_sub(step);
        self.viewport_top = self.viewport_top.saturating_sub(step);
    }

    fn jump_next_hunk(&mut self) {
        if let Some(target) = next_target(&self.hunk_targets, self.cursor_line) {
            self.jump_to(target);
        }
    }

    fn jump_previous_hunk(&mut self) {
        if let Some(target) = previous_target(&self.hunk_targets, self.cursor_line) {
            self.jump_to(target);
        }
    }

    fn jump_next_file(&mut self) {
        if let Some(target) = next_target(&self.file_targets, self.cursor_line) {
            self.jump_to(target);
        }
    }

    fn jump_previous_file(&mut self) {
        if let Some(target) = previous_target(&self.file_targets, self.cursor_line) {
            self.jump_to(target);
        }
    }

    fn jump_to(&mut self, target: usize) {
        self.cursor_line = target;
        self.viewport_top = target;
    }

    fn half_page_step(&self) -> usize {
        (self.viewport_height.max(1) / 2).max(1)
    }

    fn sync_viewport(&mut self, height: usize) {
        self.viewport_height = height.max(1);

        if self.cursor_line < self.viewport_top {
            self.viewport_top = self.cursor_line;
            return;
        }

        let viewport_bottom = self.viewport_top.saturating_add(height.saturating_sub(1));
        if self.cursor_line > viewport_bottom {
            self.viewport_top = self.cursor_line.saturating_sub(height.saturating_sub(1));
        }
    }

    fn selected_file_index(&self) -> Option<usize> {
        self.rendered_lines
            .get(self.cursor_line)
            .and_then(|line| line.file_index)
            .or_else(|| (!self.diff.is_empty()).then_some(0))
    }
}

/// Runs the read-only diff viewer until the user quits.
///
/// # Errors
///
/// Returns an error if the terminal cannot be switched into raw/alternate-screen
/// mode, if terminal drawing fails, or if event polling/reading fails.
pub fn run(diff: Diff) -> Result<(), ViewError> {
    let mut stdout = io::stdout();
    enable_raw_mode()?;
    execute!(stdout, EnterAlternateScreen)?;

    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;
    let loop_result = run_loop(&mut terminal, App::new(diff));
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
    let layout =
        Layout::horizontal([Constraint::Length(30), Constraint::Min(10)]).split(frame.area());
    let mut list_state = ListState::default();
    list_state.select(app.selected_file_index());

    let file_items = if app.diff.files.is_empty() {
        vec![ListItem::new("No files")]
    } else {
        app.diff
            .files
            .iter()
            .map(|file| ListItem::new(format!("[{}] {}", file.change, file.display_path())))
            .collect()
    };
    let file_list = List::new(file_items)
        .block(Block::default().title("Files").borders(Borders::ALL))
        .highlight_style(
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        )
        .highlight_symbol("> ");
    frame.render_stateful_widget(file_list, layout[0], &mut list_state);

    let diff_height = layout[1].height.saturating_sub(2) as usize;
    app.sync_viewport(diff_height.max(1));
    let visible_lines = app
        .rendered_lines
        .iter()
        .enumerate()
        .skip(app.viewport_top)
        .take(diff_height.max(1))
        .map(|(index, line)| line_to_text(index == app.cursor_line, line))
        .collect::<Vec<_>>();
    let diff_view =
        Paragraph::new(visible_lines).block(Block::default().title("Diff").borders(Borders::ALL));
    frame.render_widget(diff_view, layout[1]);
}

fn file_header_line(file_index: usize, file: &DiffFile) -> RenderLine {
    RenderLine {
        file_index: Some(file_index),
        kind: RenderLineKind::FileHeader,
        old_lineno: None,
        new_lineno: None,
        text: format!("{} {}", file.change, file.display_path()),
    }
}

fn hunk_header_line(file_index: usize, hunk: &Hunk) -> RenderLine {
    RenderLine {
        file_index: Some(file_index),
        kind: RenderLineKind::HunkHeader,
        old_lineno: None,
        new_lineno: None,
        text: hunk.header.clone(),
    }
}

fn diff_content_line(file_index: usize, line: &DiffLine) -> RenderLine {
    let kind = match line.kind {
        LineKind::Added => RenderLineKind::Added,
        LineKind::Removed => RenderLineKind::Removed,
        LineKind::Context => RenderLineKind::Context,
    };

    RenderLine {
        file_index: Some(file_index),
        kind,
        old_lineno: line.old_lineno,
        new_lineno: line.new_lineno,
        text: line.text.clone(),
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

fn line_to_text(is_selected: bool, line: &RenderLine) -> Line<'static> {
    let base_style = match line.kind {
        RenderLineKind::FileHeader => Style::default()
            .fg(Color::Cyan)
            .add_modifier(Modifier::BOLD),
        RenderLineKind::HunkHeader => Style::default().fg(Color::Blue),
        RenderLineKind::Added => Style::default().fg(Color::Green),
        RenderLineKind::Removed => Style::default().fg(Color::Red),
        RenderLineKind::Context => Style::default().fg(Color::DarkGray),
        RenderLineKind::Placeholder => Style::default().fg(Color::Magenta),
        RenderLineKind::EmptyState => Style::default()
            .fg(Color::DarkGray)
            .add_modifier(Modifier::ITALIC),
    };

    let style = if is_selected {
        base_style.add_modifier(Modifier::REVERSED)
    } else {
        base_style
    };

    let marker = match line.kind {
        RenderLineKind::Added => '+',
        RenderLineKind::Removed => '-',
        RenderLineKind::Placeholder => '!',
        _ => ' ',
    };

    let text = match line.kind {
        RenderLineKind::FileHeader | RenderLineKind::HunkHeader | RenderLineKind::EmptyState => {
            line.text.clone()
        }
        RenderLineKind::Added
        | RenderLineKind::Removed
        | RenderLineKind::Context
        | RenderLineKind::Placeholder => {
            format!(
                "{:>4} {:>4} {} {}",
                format_lineno(line.old_lineno),
                format_lineno(line.new_lineno),
                marker,
                line.text
            )
        }
    };

    Line::styled(text, style)
}

fn format_lineno(lineno: Option<usize>) -> String {
    lineno.map_or_else(|| " ".to_owned(), |value| value.to_string())
}

#[cfg(test)]
mod tests {
    use libframe::{Diff, DiffFile, DiffLine, FileChangeKind, Hunk, LineKind};

    use super::{App, PendingSequence};

    fn sample_diff() -> Diff {
        Diff {
            files: vec![
                DiffFile {
                    old_path: Some("src/main.rs".to_owned()),
                    new_path: Some("src/main.rs".to_owned()),
                    change: FileChangeKind::Modified,
                    has_binary_or_unrenderable_change: false,
                    hunks: vec![
                        Hunk {
                            header: "@@ -1,2 +1,3 @@".to_owned(),
                            old_start: 1,
                            old_len: 2,
                            new_start: 1,
                            new_len: 3,
                            lines: vec![
                                DiffLine {
                                    kind: LineKind::Context,
                                    old_lineno: Some(1),
                                    new_lineno: Some(1),
                                    text: "fn main() {".to_owned(),
                                },
                                DiffLine {
                                    kind: LineKind::Removed,
                                    old_lineno: Some(2),
                                    new_lineno: None,
                                    text: "    old();".to_owned(),
                                },
                                DiffLine {
                                    kind: LineKind::Added,
                                    old_lineno: None,
                                    new_lineno: Some(2),
                                    text: "    new();".to_owned(),
                                },
                            ],
                        },
                        Hunk {
                            header: "@@ -8,1 +9,1 @@".to_owned(),
                            old_start: 8,
                            old_len: 1,
                            new_start: 9,
                            new_len: 1,
                            lines: vec![DiffLine {
                                kind: LineKind::Added,
                                old_lineno: None,
                                new_lineno: Some(9),
                                text: "tail();".to_owned(),
                            }],
                        },
                    ],
                },
                DiffFile {
                    old_path: Some("src/lib.rs".to_owned()),
                    new_path: Some("src/lib.rs".to_owned()),
                    change: FileChangeKind::Modified,
                    has_binary_or_unrenderable_change: false,
                    hunks: vec![Hunk {
                        header: "@@ -1,1 +1,1 @@".to_owned(),
                        old_start: 1,
                        old_len: 1,
                        new_start: 1,
                        new_len: 1,
                        lines: vec![DiffLine {
                            kind: LineKind::Added,
                            old_lineno: None,
                            new_lineno: Some(1),
                            text: "pub fn helper() {}".to_owned(),
                        }],
                    }],
                },
            ],
        }
    }

    #[test]
    fn navigation_clamps_at_bounds() {
        let mut app = App::new(sample_diff());
        app.move_up();
        assert_eq!(app.cursor_line, 0);

        for _ in 0..100 {
            app.move_down();
        }

        assert_eq!(app.cursor_line, app.rendered_lines.len() - 1);
    }

    #[test]
    fn jumps_between_files_and_hunks() {
        let mut app = App::new(sample_diff());
        assert_eq!(app.file_targets, vec![0, 7]);
        assert_eq!(app.hunk_targets, vec![1, 5, 8]);

        app.jump_next_hunk();
        assert_eq!(app.cursor_line, 1);
        app.jump_next_hunk();
        assert_eq!(app.cursor_line, 5);
        app.jump_next_file();
        assert_eq!(app.cursor_line, 7);
        app.jump_previous_file();
        assert_eq!(app.cursor_line, 0);
    }

    #[test]
    fn jumps_anchor_the_target_at_the_top_of_the_viewport() {
        let mut app = App::new(sample_diff());
        app.viewport_top = 3;

        app.jump_next_hunk();
        assert_eq!(app.cursor_line, 1);
        assert_eq!(app.viewport_top, 1);

        app.jump_next_file();
        assert_eq!(app.cursor_line, 7);
        assert_eq!(app.viewport_top, 7);
    }

    #[test]
    fn half_page_navigation_moves_cursor_and_viewport() {
        let mut app = App::new(sample_diff());
        app.viewport_height = 4;

        app.move_half_page_down();
        assert_eq!(app.cursor_line, 2);
        assert_eq!(app.viewport_top, 2);

        app.move_half_page_down();
        assert_eq!(app.cursor_line, 4);
        assert_eq!(app.viewport_top, 4);

        app.move_half_page_up();
        assert_eq!(app.cursor_line, 2);
        assert_eq!(app.viewport_top, 2);
    }

    #[test]
    fn handles_empty_diffs_without_panicking() {
        let mut app = App::new(Diff::default());
        assert_eq!(app.rendered_lines.len(), 1);
        assert!(app.selected_file_index().is_none());
        assert_eq!(app.pending_sequence, PendingSequence::None);
        app.move_down();
        assert_eq!(app.cursor_line, 0);
    }
}
