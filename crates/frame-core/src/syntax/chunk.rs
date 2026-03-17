use tree_sitter::{Node, Parser};
use unicode_width::UnicodeWidthChar;

use crate::review::CodeBuffer;

use super::LanguageId;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct BufferPoint {
    pub line: usize,
    pub byte_col: usize,
    pub display_col: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BufferSpan {
    pub start: BufferPoint,
    pub end: BufferPoint,
}

impl BufferSpan {
    #[must_use]
    pub fn normalized(self) -> Self {
        if self.start <= self.end {
            self
        } else {
            Self {
                start: self.end,
                end: self.start,
            }
        }
    }

    #[must_use]
    pub fn intersects_line(self, line: usize) -> bool {
        let span = self.normalized();
        span.start.line <= line && line <= span.end.line
    }

    #[must_use]
    pub fn contains_line_byte(self, line: usize, byte_col: usize) -> bool {
        let span = self.normalized();
        if !span.intersects_line(line) {
            return false;
        }

        if span.start.line == span.end.line {
            return span.start.byte_col <= byte_col && byte_col < span.end.byte_col;
        }

        if line == span.start.line {
            return span.start.byte_col <= byte_col;
        }

        if line == span.end.line {
            return byte_col < span.end.byte_col;
        }

        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChunkKind {
    Keyword,
    Identifier,
    Type,
    Parameter,
    Field,
    Literal,
    Operator,
    Text,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChunkRole {
    Keyword,
    Symbol,
    Type,
    Parameter,
    Field,
    CallTarget,
    Literal,
    Operator,
    Text,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NavigableChunk {
    pub span: BufferSpan,
    pub kind: ChunkKind,
    pub role: ChunkRole,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct NodeSegmentSpec {
    start_line: usize,
    end_line: usize,
    start_col: usize,
    end_col: usize,
    kind: ChunkKind,
    role: ChunkRole,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ChunkedLine {
    pub chunks: Vec<NavigableChunk>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ChunkedFile {
    pub lines: Vec<ChunkedLine>,
}

impl ChunkedFile {
    #[must_use]
    pub fn empty(line_count: usize) -> Self {
        Self {
            lines: vec![ChunkedLine::default(); line_count.max(1)],
        }
    }

    #[must_use]
    pub fn line(&self, index: usize) -> Option<&ChunkedLine> {
        self.lines.get(index)
    }

    #[must_use]
    pub fn chunk(&self, line: usize, index: usize) -> Option<&NavigableChunk> {
        self.line(line)?.chunks.get(index)
    }

    #[must_use]
    pub fn first_chunk_index(&self, line: usize) -> Option<usize> {
        (!self.line(line)?.chunks.is_empty()).then_some(0)
    }

    #[must_use]
    pub fn last_chunk_index(&self, line: usize) -> Option<usize> {
        self.line(line)?.chunks.len().checked_sub(1)
    }

    #[must_use]
    pub fn nearest_chunk_index(&self, line: usize, display_col: usize) -> Option<usize> {
        let chunks = &self.line(line)?.chunks;
        let mut best = None;
        let mut best_distance = usize::MAX;

        for (index, chunk) in chunks.iter().enumerate() {
            let distance = chunk.span.start.display_col.abs_diff(display_col);
            if distance < best_distance {
                best = Some(index);
                best_distance = distance;
            }
        }

        best
    }
}

#[must_use]
pub fn chunk_buffer(language: Option<LanguageId>, buffer: &CodeBuffer) -> ChunkedFile {
    if matches!(language, Some(LanguageId::Markdown)) {
        return lexical_chunks(buffer);
    }

    if let Some(language) = language
        && let Some(parsed) = parser_chunks(language, buffer)
        && parsed.lines.iter().any(|line| !line.chunks.is_empty())
    {
        return parsed;
    }

    lexical_chunks(buffer)
}

fn parser_chunks(language: LanguageId, buffer: &CodeBuffer) -> Option<ChunkedFile> {
    let source = buffer.to_source();
    let mut parser = Parser::new();
    parser.set_language(&language_for(language)).ok()?;
    let tree = parser.parse(source.as_str(), None)?;
    let mut chunked = ChunkedFile::empty(buffer.line_count());
    let mut leaves = Vec::new();

    collect_leaf_nodes(tree.root_node(), &mut leaves);

    for node in leaves {
        let range = node.range();
        if range.start_byte == range.end_byte {
            continue;
        }

        let text = source.get(range.start_byte..range.end_byte)?;
        let Some((kind, role)) = classify_node(language, node, text) else {
            continue;
        };

        push_node_segments(
            &mut chunked,
            buffer,
            NodeSegmentSpec {
                start_line: range.start_point.row,
                end_line: range.end_point.row,
                start_col: range.start_point.column,
                end_col: range.end_point.column,
                kind,
                role,
            },
        );
    }

    dedup_and_sort(&mut chunked);
    Some(chunked)
}

fn collect_leaf_nodes<'tree>(node: Node<'tree>, leaves: &mut Vec<Node<'tree>>) {
    if node.is_error() || node.is_missing() {
        return;
    }

    if node.child_count() == 0 {
        leaves.push(node);
        return;
    }

    let mut cursor = node.walk();
    for child in node.children(&mut cursor) {
        collect_leaf_nodes(child, leaves);
    }
}

fn push_node_segments(chunked: &mut ChunkedFile, buffer: &CodeBuffer, spec: NodeSegmentSpec) {
    if spec.start_line == spec.end_line {
        if let Some(span) = line_span(buffer, spec.start_line, spec.start_col, spec.end_col) {
            push_chunk(chunked, spec.kind, spec.role, span);
        }
        return;
    }

    for line in spec.start_line..=spec.end_line {
        let Some(line_text) = buffer.line(line) else {
            break;
        };
        let line_start = if line == spec.start_line {
            spec.start_col
        } else {
            0
        };
        let line_end = if line == spec.end_line {
            spec.end_col
        } else {
            line_text.len()
        };

        if let Some(span) = line_span(buffer, line, line_start, line_end) {
            push_chunk(chunked, spec.kind, spec.role, span);
        }
    }
}

fn line_span(
    buffer: &CodeBuffer,
    line: usize,
    start_byte: usize,
    end_byte: usize,
) -> Option<BufferSpan> {
    let line_text = buffer.line(line)?;
    let start_byte = start_byte.min(line_text.len());
    let end_byte = end_byte.min(line_text.len());
    if start_byte >= end_byte
        || !line_text.is_char_boundary(start_byte)
        || !line_text.is_char_boundary(end_byte)
    {
        return None;
    }

    Some(BufferSpan {
        start: BufferPoint {
            line,
            byte_col: start_byte,
            display_col: display_col(line_text, start_byte),
        },
        end: BufferPoint {
            line,
            byte_col: end_byte,
            display_col: display_col(line_text, end_byte),
        },
    })
}

fn push_chunk(chunked: &mut ChunkedFile, kind: ChunkKind, role: ChunkRole, span: BufferSpan) {
    if let Some(line) = chunked.lines.get_mut(span.start.line) {
        line.chunks.push(NavigableChunk { span, kind, role });
    }
}

fn dedup_and_sort(chunked: &mut ChunkedFile) {
    for line in &mut chunked.lines {
        line.chunks.sort_by(|left, right| {
            left.span
                .start
                .byte_col
                .cmp(&right.span.start.byte_col)
                .then(left.span.end.byte_col.cmp(&right.span.end.byte_col))
        });
        line.chunks.dedup_by(|left, right| {
            left.span == right.span && left.kind == right.kind && left.role == right.role
        });
    }
}

fn lexical_chunks(buffer: &CodeBuffer) -> ChunkedFile {
    let mut chunked = ChunkedFile::empty(buffer.line_count());

    for (line_index, line_text) in buffer.lines().iter().enumerate() {
        let mut byte = 0usize;
        while byte < line_text.len() {
            let ch = line_text[byte..]
                .chars()
                .next()
                .expect("slice is non-empty");

            if ch.is_whitespace() {
                byte += ch.len_utf8();
                continue;
            }

            let end = if is_identifier_start(ch) {
                consume_while(line_text, byte, |value| {
                    value.is_alphanumeric() || value == '_'
                })
            } else if ch.is_ascii_digit() {
                consume_while(line_text, byte, |value| {
                    value.is_ascii_digit() || value == '_'
                })
            } else if ch == '"' || ch == '\'' {
                consume_string(line_text, byte, ch)
            } else if is_operator_char(ch) {
                consume_while(line_text, byte, is_operator_char)
            } else {
                byte += ch.len_utf8();
                continue;
            };

            let text = &line_text[byte..end];
            let (kind, role) = classify_lexical_chunk(text);
            push_chunk(
                &mut chunked,
                kind,
                role,
                BufferSpan {
                    start: BufferPoint {
                        line: line_index,
                        byte_col: byte,
                        display_col: display_col(line_text, byte),
                    },
                    end: BufferPoint {
                        line: line_index,
                        byte_col: end,
                        display_col: display_col(line_text, end),
                    },
                },
            );

            byte = end;
        }
    }

    chunked
}

fn classify_node(
    language: LanguageId,
    node: Node<'_>,
    text: &str,
) -> Option<(ChunkKind, ChunkRole)> {
    let text = text.trim();
    if text.is_empty() || should_skip_token(text) {
        return None;
    }

    let kind_name = node.kind();
    if is_keyword(language, text) {
        return Some((ChunkKind::Keyword, ChunkRole::Keyword));
    }

    if is_type_node(kind_name) {
        return Some((ChunkKind::Type, ChunkRole::Type));
    }

    if is_literal_node(kind_name, text) {
        return Some((ChunkKind::Literal, ChunkRole::Literal));
    }

    if is_operator_token(text) {
        return Some((ChunkKind::Operator, ChunkRole::Operator));
    }

    if kind_name == "field_identifier" {
        return Some((ChunkKind::Field, ChunkRole::Field));
    }

    if is_identifier_text(text) {
        if has_ancestor_kind(node, "parameter") {
            return Some((ChunkKind::Parameter, ChunkRole::Parameter));
        }

        if has_ancestor_kind(node, "field") {
            return Some((ChunkKind::Field, ChunkRole::Field));
        }

        if has_ancestor_kind(node, "call") {
            return Some((ChunkKind::Identifier, ChunkRole::CallTarget));
        }

        if has_ancestor_kind(node, "type") {
            return Some((ChunkKind::Type, ChunkRole::Type));
        }

        return Some((ChunkKind::Identifier, ChunkRole::Symbol));
    }

    Some((ChunkKind::Text, ChunkRole::Text))
}

fn has_ancestor_kind(mut node: Node<'_>, needle: &str) -> bool {
    while let Some(parent) = node.parent() {
        if parent.kind().contains(needle) {
            return true;
        }
        node = parent;
    }

    false
}

fn is_keyword(language: LanguageId, text: &str) -> bool {
    match language {
        LanguageId::Rust => matches!(
            text,
            "as" | "async"
                | "await"
                | "break"
                | "const"
                | "continue"
                | "crate"
                | "dyn"
                | "else"
                | "enum"
                | "extern"
                | "false"
                | "fn"
                | "for"
                | "if"
                | "impl"
                | "in"
                | "let"
                | "loop"
                | "match"
                | "mod"
                | "move"
                | "mut"
                | "pub"
                | "ref"
                | "return"
                | "self"
                | "Self"
                | "static"
                | "struct"
                | "super"
                | "trait"
                | "true"
                | "type"
                | "unsafe"
                | "use"
                | "where"
                | "while"
        ),
        LanguageId::Toml | LanguageId::Markdown => false,
    }
}

fn is_type_node(kind: &str) -> bool {
    matches!(kind, "primitive_type" | "type_identifier") || kind.contains("type")
}

fn is_literal_node(kind: &str, text: &str) -> bool {
    kind.contains("literal")
        || kind.contains("string")
        || kind.contains("char")
        || kind.contains("number")
        || kind.contains("integer")
        || kind.contains("float")
        || text.starts_with('"')
        || text.starts_with('\'')
        || text.chars().all(|ch| ch.is_ascii_digit() || ch == '_')
}

fn should_skip_token(text: &str) -> bool {
    matches!(
        text,
        "(" | ")" | "{" | "}" | "[" | "]" | "," | ";" | ":" | "." | "->" | "::"
    )
}

fn is_operator_token(text: &str) -> bool {
    !text.is_empty()
        && text.chars().all(is_operator_char)
        && !matches!(text, "(" | ")" | "{" | "}" | "[" | "]" | "," | ";" | ":")
}

fn is_identifier_text(text: &str) -> bool {
    let mut chars = text.chars();
    let Some(first) = chars.next() else {
        return false;
    };

    is_identifier_start(first) && chars.all(|ch| ch.is_alphanumeric() || ch == '_')
}

fn classify_lexical_chunk(text: &str) -> (ChunkKind, ChunkRole) {
    if text.starts_with('"')
        || text.starts_with('\'')
        || text.chars().all(|ch| ch.is_ascii_digit() || ch == '_')
    {
        return (ChunkKind::Literal, ChunkRole::Literal);
    }

    if is_operator_token(text) {
        return (ChunkKind::Operator, ChunkRole::Operator);
    }

    (ChunkKind::Identifier, ChunkRole::Symbol)
}

fn consume_while<F>(line: &str, start: usize, predicate: F) -> usize
where
    F: Fn(char) -> bool,
{
    let mut end = start;
    for (offset, ch) in line[start..].char_indices() {
        if !predicate(ch) {
            break;
        }
        end = start + offset + ch.len_utf8();
    }
    end
}

fn consume_string(line: &str, start: usize, quote: char) -> usize {
    let mut escaped = false;
    for (offset, ch) in line[start + quote.len_utf8()..].char_indices() {
        if escaped {
            escaped = false;
            continue;
        }

        if ch == '\\' {
            escaped = true;
            continue;
        }

        if ch == quote {
            return start + quote.len_utf8() + offset + ch.len_utf8();
        }
    }

    line.len()
}

fn is_identifier_start(ch: char) -> bool {
    ch == '_' || ch.is_alphabetic()
}

fn is_operator_char(ch: char) -> bool {
    matches!(
        ch,
        '=' | '+' | '-' | '*' | '/' | '%' | '!' | '<' | '>' | '&' | '|' | '^' | '~' | '?'
    )
}

const DEFAULT_TAB_SIZE: usize = 4;

fn display_col(line: &str, byte_col: usize) -> usize {
    display_col_with_tab_size(line, byte_col, DEFAULT_TAB_SIZE)
}

fn display_col_with_tab_size(line: &str, byte_col: usize, tab_size: usize) -> usize {
    let clamped = byte_col.min(line.len());
    let tab_size = tab_size.max(1);
    let mut width = 0;

    for (offset, ch) in line.char_indices() {
        if offset >= clamped {
            break;
        }

        if ch == '\t' {
            width += tab_size - (width % tab_size);
            continue;
        }

        width += ch.width().unwrap_or(0);
    }

    width
}

fn language_for(language: LanguageId) -> tree_sitter::Language {
    match language {
        LanguageId::Rust => tree_sitter_rust::LANGUAGE.into(),
        LanguageId::Toml => tree_sitter_toml_ng::LANGUAGE.into(),
        LanguageId::Markdown => tree_sitter_md::LANGUAGE.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::{
        BufferSpan, ChunkKind, ChunkRole, chunk_buffer, classify_lexical_chunk, display_col,
        display_col_with_tab_size, lexical_chunks,
    };
    use crate::{CodeBuffer, LanguageId};

    #[test]
    fn parser_chunks_follow_rust_signature_structure() {
        let chunks = chunk_buffer(
            Some(LanguageId::Rust),
            &CodeBuffer::from_text("fn add(x: i32, y: i32) -> i32 {\n    x + y\n}\n"),
        );
        let first_line = chunks.line(0).expect("line exists");
        let kinds = first_line
            .chunks
            .iter()
            .map(|chunk| chunk.kind)
            .collect::<Vec<_>>();

        assert!(kinds.contains(&ChunkKind::Keyword));
        assert!(kinds.contains(&ChunkKind::Identifier));
        assert!(kinds.contains(&ChunkKind::Parameter));
        assert!(kinds.contains(&ChunkKind::Type));
    }

    #[test]
    fn lexical_fallback_extracts_identifiers_literals_and_operators() {
        let chunks = lexical_chunks(&CodeBuffer::from_text("alpha = beta + 42\n"));
        let first_line = chunks.line(0).expect("line exists");
        let roles = first_line
            .chunks
            .iter()
            .map(|chunk| chunk.role)
            .collect::<Vec<_>>();

        assert!(roles.contains(&ChunkRole::Symbol));
        assert!(roles.contains(&ChunkRole::Literal));
        assert!(roles.contains(&ChunkRole::Operator));
    }

    #[test]
    fn buffer_span_normalizes_backward_ranges() {
        let span = BufferSpan {
            start: super::BufferPoint {
                line: 2,
                byte_col: 8,
                display_col: 8,
            },
            end: super::BufferPoint {
                line: 1,
                byte_col: 4,
                display_col: 4,
            },
        }
        .normalized();

        assert_eq!(span.start.line, 1);
        assert_eq!(span.end.line, 2);
    }

    #[test]
    fn lexical_classification_prefers_literals_and_operators() {
        assert_eq!(
            classify_lexical_chunk("\"frame\""),
            (ChunkKind::Literal, ChunkRole::Literal)
        );
        assert_eq!(
            classify_lexical_chunk("=="),
            (ChunkKind::Operator, ChunkRole::Operator)
        );
    }

    #[test]
    fn display_col_uses_terminal_tab_stops() {
        let line = "\ta\tz";
        let z_byte = line.rfind('z').expect("z present");

        assert_eq!(display_col_with_tab_size(line, z_byte, 4), 8);
    }

    #[test]
    fn display_col_counts_wide_glyph_cells() {
        let line = "a界z";
        let z_byte = line.rfind('z').expect("z present");

        assert_eq!(display_col(line, z_byte), 3);
    }
}
