use std::fmt;

use thiserror::Error;

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct PatchSet {
    pub files: Vec<PatchFile>,
}

impl PatchSet {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.files.is_empty()
    }

    #[must_use]
    pub fn file_count(&self) -> usize {
        self.files.len()
    }

    #[must_use]
    pub fn hunk_count(&self) -> usize {
        self.files.iter().map(|file| file.hunks.len()).sum()
    }

    #[must_use]
    pub fn changed_line_count(&self) -> usize {
        self.files
            .iter()
            .flat_map(|file| file.hunks.iter())
            .flat_map(|hunk| hunk.lines.iter())
            .filter(|line| matches!(line.kind, LineKind::Added | LineKind::Removed))
            .count()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchFile {
    pub old_path: Option<String>,
    pub new_path: Option<String>,
    pub change: FileChangeKind,
    pub hunks: Vec<PatchHunk>,
    pub has_binary_or_unrenderable_change: bool,
}

impl Default for PatchFile {
    fn default() -> Self {
        Self {
            old_path: None,
            new_path: None,
            change: FileChangeKind::Modified,
            hunks: Vec::new(),
            has_binary_or_unrenderable_change: false,
        }
    }
}

impl PatchFile {
    #[must_use]
    pub fn display_path(&self) -> &str {
        self.new_path
            .as_deref()
            .or(self.old_path.as_deref())
            .unwrap_or("<unknown>")
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileChangeKind {
    Added,
    Deleted,
    Modified,
    Renamed,
}

impl fmt::Display for FileChangeKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let label = match self {
            Self::Added => "A",
            Self::Deleted => "D",
            Self::Modified => "M",
            Self::Renamed => "R",
        };

        f.write_str(label)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchHunk {
    pub header: String,
    pub old_start: usize,
    pub old_len: usize,
    pub new_start: usize,
    pub new_len: usize,
    pub lines: Vec<PatchLine>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchLine {
    pub kind: LineKind,
    pub old_lineno: Option<usize>,
    pub new_lineno: Option<usize>,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LineKind {
    Added,
    Removed,
    Context,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum PatchParseError {
    #[error("malformed hunk header: {0}")]
    MalformedHunkHeader(String),
    #[error("encountered diff content before a hunk header: {0}")]
    DiffLineOutsideHunk(String),
}

#[derive(Debug)]
struct ActiveHunk {
    hunk: PatchHunk,
    next_old_lineno: usize,
    next_new_lineno: usize,
}

#[derive(Debug, Default)]
struct PatchParser {
    patch_set: PatchSet,
    current_file: Option<PatchFile>,
    current_hunk: Option<ActiveHunk>,
}

impl PatchParser {
    fn parse(mut self, input: &str) -> Result<PatchSet, PatchParseError> {
        for line in input.lines() {
            self.push_line(line)?;
        }

        self.finish_current_hunk();
        self.finish_current_file();

        Ok(self.patch_set)
    }

    fn push_line(&mut self, line: &str) -> Result<(), PatchParseError> {
        if let Some(rest) = line.strip_prefix("diff --git ") {
            self.finish_current_hunk();
            self.finish_current_file();

            let (old_path, new_path) = parse_diff_git_paths(rest);
            self.current_file = Some(PatchFile {
                old_path,
                new_path,
                ..PatchFile::default()
            });
            return Ok(());
        }

        let Some(file) = self.current_file.as_mut() else {
            return Ok(());
        };

        if line.starts_with("@@") {
            self.finish_current_hunk();
            self.current_hunk = Some(parse_hunk_header(line)?);
            return Ok(());
        }

        if handle_file_metadata(file, line) {
            return Ok(());
        }

        if let Some(hunk) = self.current_hunk.as_mut() {
            push_hunk_line(hunk, line);
            return Ok(());
        }

        if matches!(line.chars().next(), Some('+' | '-' | ' ' | '\\')) {
            return Err(PatchParseError::DiffLineOutsideHunk(line.to_owned()));
        }

        Ok(())
    }

    fn finish_current_hunk(&mut self) {
        if let Some(active_hunk) = self.current_hunk.take()
            && let Some(file) = self.current_file.as_mut()
        {
            file.hunks.push(active_hunk.hunk);
        }
    }

    fn finish_current_file(&mut self) {
        if let Some(file) = self.current_file.take() {
            self.patch_set.files.push(file);
        }
    }
}

/// Parses unified diff text into a typed patch set.
///
/// # Errors
///
/// Returns an error if a hunk header is malformed or if diff content appears
/// outside a hunk.
pub fn parse_patch(input: &str) -> Result<PatchSet, PatchParseError> {
    PatchParser::default().parse(input)
}

fn handle_file_metadata(file: &mut PatchFile, line: &str) -> bool {
    if let Some(path) = line.strip_prefix("--- ") {
        file.old_path = normalize_patch_path(path);
        return true;
    }

    if let Some(path) = line.strip_prefix("+++ ") {
        file.new_path = normalize_patch_path(path);
        return true;
    }

    if line == "GIT binary patch" || line.starts_with("Binary files ") {
        file.has_binary_or_unrenderable_change = true;
        return true;
    }

    if line.starts_with("new file mode ") {
        file.change = FileChangeKind::Added;
        return true;
    }

    if line.starts_with("deleted file mode ") {
        file.change = FileChangeKind::Deleted;
        return true;
    }

    if let Some(path) = line.strip_prefix("rename from ") {
        file.change = FileChangeKind::Renamed;
        file.old_path = Some(path.to_owned());
        return true;
    }

    if let Some(path) = line.strip_prefix("rename to ") {
        file.change = FileChangeKind::Renamed;
        file.new_path = Some(path.to_owned());
        return true;
    }

    line.starts_with("similarity index ")
        || line.starts_with("index ")
        || line.starts_with("old mode ")
        || line.starts_with("new mode ")
        || line.starts_with("copy from ")
        || line.starts_with("copy to ")
}

fn push_hunk_line(hunk: &mut ActiveHunk, line: &str) {
    match line.chars().next() {
        Some('+') => {
            hunk.hunk.lines.push(PatchLine {
                kind: LineKind::Added,
                old_lineno: None,
                new_lineno: Some(hunk.next_new_lineno),
                text: line[1..].to_owned(),
            });
            hunk.next_new_lineno += 1;
        }
        Some('-') => {
            hunk.hunk.lines.push(PatchLine {
                kind: LineKind::Removed,
                old_lineno: Some(hunk.next_old_lineno),
                new_lineno: None,
                text: line[1..].to_owned(),
            });
            hunk.next_old_lineno += 1;
        }
        Some(' ') => {
            hunk.hunk.lines.push(PatchLine {
                kind: LineKind::Context,
                old_lineno: Some(hunk.next_old_lineno),
                new_lineno: Some(hunk.next_new_lineno),
                text: line[1..].to_owned(),
            });
            hunk.next_old_lineno += 1;
            hunk.next_new_lineno += 1;
        }
        Some('\\') if line == r"\ No newline at end of file" => {
            hunk.hunk.lines.push(PatchLine {
                kind: LineKind::Context,
                old_lineno: None,
                new_lineno: None,
                text: line.to_owned(),
            });
        }
        _ => {}
    }
}

fn parse_diff_git_paths(rest: &str) -> (Option<String>, Option<String>) {
    let mut parts = rest.split_whitespace();
    let old = parts.next().and_then(normalize_patch_path);
    let new = parts.next().and_then(normalize_patch_path);
    (old, new)
}

fn normalize_patch_path(raw: &str) -> Option<String> {
    let raw = raw.trim();

    if raw == "/dev/null" {
        return None;
    }

    let unquoted = raw
        .strip_prefix('"')
        .and_then(|value| value.strip_suffix('"'))
        .unwrap_or(raw);
    let path = unquoted
        .strip_prefix("a/")
        .or_else(|| unquoted.strip_prefix("b/"))
        .unwrap_or(unquoted);

    Some(path.to_owned())
}

fn parse_hunk_header(line: &str) -> Result<ActiveHunk, PatchParseError> {
    let mut parts = line.split("@@");
    let _ = parts.next();
    let ranges = parts
        .next()
        .map(str::trim)
        .ok_or_else(|| PatchParseError::MalformedHunkHeader(line.to_owned()))?;
    let header_suffix = parts.next().map(str::trim).unwrap_or_default();

    let mut range_parts = ranges.split_whitespace();
    let old_range = range_parts
        .next()
        .ok_or_else(|| PatchParseError::MalformedHunkHeader(line.to_owned()))?;
    let new_range = range_parts
        .next()
        .ok_or_else(|| PatchParseError::MalformedHunkHeader(line.to_owned()))?;

    let (old_start, old_len) = parse_range(old_range, '-')?;
    let (new_start, new_len) = parse_range(new_range, '+')?;

    let header = if header_suffix.is_empty() {
        line.to_owned()
    } else {
        format!("@@ {ranges} @@ {header_suffix}")
    };

    Ok(ActiveHunk {
        hunk: PatchHunk {
            header,
            old_start,
            old_len,
            new_start,
            new_len,
            lines: Vec::new(),
        },
        next_old_lineno: old_start,
        next_new_lineno: new_start,
    })
}

fn parse_range(input: &str, prefix: char) -> Result<(usize, usize), PatchParseError> {
    let value = input
        .strip_prefix(prefix)
        .ok_or_else(|| PatchParseError::MalformedHunkHeader(input.to_owned()))?;
    let mut parts = value.split(',');
    let start = parts
        .next()
        .and_then(|part| part.parse::<usize>().ok())
        .ok_or_else(|| PatchParseError::MalformedHunkHeader(input.to_owned()))?;
    let len = parts
        .next()
        .map(|part| {
            part.parse::<usize>()
                .map_err(|_| PatchParseError::MalformedHunkHeader(input.to_owned()))
        })
        .transpose()?
        .unwrap_or(1);

    Ok((start, len))
}

#[cfg(test)]
mod tests {
    use super::{FileChangeKind, LineKind, parse_patch};

    #[test]
    fn parses_empty_patch() {
        let patch = parse_patch("").expect("empty patch should parse");
        assert!(patch.is_empty());
        assert_eq!(patch.file_count(), 0);
        assert_eq!(patch.hunk_count(), 0);
        assert_eq!(patch.changed_line_count(), 0);
    }

    #[test]
    fn parses_multiple_files_and_hunks() {
        let input = r#"diff --git a/src/main.rs b/src/main.rs
index 1111111..2222222 100644
--- a/src/main.rs
+++ b/src/main.rs
@@ -1,3 +1,4 @@ fn main() {
 fn main() {
-    println!("old");
+    println!("new");
+    println!("second");
 }
@@ -10 +11 @@ fn later() {
-    value();
+    other();
diff --git a/src/lib.rs b/src/lib.rs
index 3333333..4444444 100644
--- a/src/lib.rs
+++ b/src/lib.rs
@@ -1 +1 @@
-pub fn old() {}
+pub fn new() {}
"#;

        let patch = parse_patch(input).expect("multi-file patch should parse");
        assert_eq!(patch.file_count(), 2);
        assert_eq!(patch.hunk_count(), 3);
        assert_eq!(patch.changed_line_count(), 7);
        assert_eq!(patch.files[0].display_path(), "src/main.rs");
        assert_eq!(patch.files[1].display_path(), "src/lib.rs");
    }

    #[test]
    fn parses_added_deleted_and_renamed_files() {
        let input = r"diff --git a/dev/null b/new.txt
new file mode 100644
--- /dev/null
+++ b/new.txt
@@ -0,0 +1 @@
+hello
diff --git a/old.txt b/dev/null
deleted file mode 100644
--- a/old.txt
+++ /dev/null
@@ -1 +0,0 @@
-goodbye
diff --git a/a.txt b/b.txt
similarity index 90%
rename from a.txt
rename to b.txt
@@ -1 +1 @@
-left
+right
";

        let patch = parse_patch(input).expect("metadata patch should parse");
        assert_eq!(patch.files[0].change, FileChangeKind::Added);
        assert_eq!(patch.files[1].change, FileChangeKind::Deleted);
        assert_eq!(patch.files[2].change, FileChangeKind::Renamed);
    }

    #[test]
    fn tracks_line_numbers_inside_hunks() {
        let input = r"diff --git a/file.txt b/file.txt
--- a/file.txt
+++ b/file.txt
@@ -3,2 +3,3 @@
 context
-removed
+added
+also added
";

        let patch = parse_patch(input).expect("line numbers should parse");
        let lines = &patch.files[0].hunks[0].lines;
        assert_eq!(lines[0].kind, LineKind::Context);
        assert_eq!(lines[0].old_lineno, Some(3));
        assert_eq!(lines[0].new_lineno, Some(3));
        assert_eq!(lines[1].old_lineno, Some(4));
        assert_eq!(lines[1].new_lineno, None);
        assert_eq!(lines[2].old_lineno, None);
        assert_eq!(lines[2].new_lineno, Some(4));
        assert_eq!(lines[3].new_lineno, Some(5));
    }
}
