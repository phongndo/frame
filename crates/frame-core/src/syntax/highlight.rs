use std::sync::OnceLock;

use tree_sitter_highlight::{Highlight, HighlightConfiguration, HighlightEvent, Highlighter};

use crate::review::CodeBuffer;

use super::LanguageId;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HighlightStyleKey {
    Attribute,
    Comment,
    Constant,
    ConstantBuiltin,
    Constructor,
    Embedded,
    Function,
    FunctionBuiltin,
    Keyword,
    Module,
    Number,
    Operator,
    Property,
    PropertyBuiltin,
    Punctuation,
    PunctuationBracket,
    PunctuationDelimiter,
    PunctuationSpecial,
    String,
    StringEscape,
    StringSpecial,
    Tag,
    TextEmphasis,
    TextLiteral,
    TextReference,
    TextStrong,
    TextTitle,
    TextUri,
    Type,
    TypeBuiltin,
    Variable,
    VariableBuiltin,
    VariableParameter,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HighlightSpan {
    pub start_byte: usize,
    pub end_byte: usize,
    pub style: HighlightStyleKey,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HighlightedLine {
    pub spans: Vec<HighlightSpan>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HighlightedFile {
    pub lines: Vec<HighlightedLine>,
}

impl HighlightedFile {
    #[must_use]
    pub fn line(&self, index: usize) -> Option<&HighlightedLine> {
        self.lines.get(index)
    }
}

struct SyntaxRuntime {
    rust: HighlightConfiguration,
    toml: HighlightConfiguration,
    markdown: HighlightConfiguration,
    markdown_inline: HighlightConfiguration,
}

#[derive(Debug, Clone, Copy)]
struct LineMetrics {
    start_byte: usize,
    end_byte: usize,
}

const HIGHLIGHT_NAMES: &[&str] = &[
    "attribute",
    "comment",
    "constant",
    "constant.builtin",
    "constructor",
    "embedded",
    "function",
    "function.builtin",
    "keyword",
    "module",
    "number",
    "operator",
    "property",
    "property.builtin",
    "punctuation",
    "punctuation.bracket",
    "punctuation.delimiter",
    "punctuation.special",
    "string",
    "string.escape",
    "string.special",
    "tag",
    "text.emphasis",
    "text.literal",
    "text.reference",
    "text.strong",
    "text.title",
    "text.uri",
    "type",
    "type.builtin",
    "variable",
    "variable.builtin",
    "variable.parameter",
];

const STYLE_KEYS: &[HighlightStyleKey] = &[
    HighlightStyleKey::Attribute,
    HighlightStyleKey::Comment,
    HighlightStyleKey::Constant,
    HighlightStyleKey::ConstantBuiltin,
    HighlightStyleKey::Constructor,
    HighlightStyleKey::Embedded,
    HighlightStyleKey::Function,
    HighlightStyleKey::FunctionBuiltin,
    HighlightStyleKey::Keyword,
    HighlightStyleKey::Module,
    HighlightStyleKey::Number,
    HighlightStyleKey::Operator,
    HighlightStyleKey::Property,
    HighlightStyleKey::PropertyBuiltin,
    HighlightStyleKey::Punctuation,
    HighlightStyleKey::PunctuationBracket,
    HighlightStyleKey::PunctuationDelimiter,
    HighlightStyleKey::PunctuationSpecial,
    HighlightStyleKey::String,
    HighlightStyleKey::StringEscape,
    HighlightStyleKey::StringSpecial,
    HighlightStyleKey::Tag,
    HighlightStyleKey::TextEmphasis,
    HighlightStyleKey::TextLiteral,
    HighlightStyleKey::TextReference,
    HighlightStyleKey::TextStrong,
    HighlightStyleKey::TextTitle,
    HighlightStyleKey::TextUri,
    HighlightStyleKey::Type,
    HighlightStyleKey::TypeBuiltin,
    HighlightStyleKey::Variable,
    HighlightStyleKey::VariableBuiltin,
    HighlightStyleKey::VariableParameter,
];

static RUNTIME: OnceLock<Result<SyntaxRuntime, String>> = OnceLock::new();

#[must_use]
pub fn highlight_buffer(language: LanguageId, buffer: &CodeBuffer) -> Option<HighlightedFile> {
    let runtime = runtime()?;
    runtime.highlight(language, buffer)
}

fn runtime() -> Option<&'static SyntaxRuntime> {
    RUNTIME.get_or_init(SyntaxRuntime::new).as_ref().ok()
}

impl SyntaxRuntime {
    fn new() -> Result<Self, String> {
        let mut rust = HighlightConfiguration::new(
            tree_sitter_rust::LANGUAGE.into(),
            "rust",
            tree_sitter_rust::HIGHLIGHTS_QUERY,
            tree_sitter_rust::INJECTIONS_QUERY,
            "",
        )
        .map_err(|error| error.to_string())?;
        rust.configure(HIGHLIGHT_NAMES);

        let mut toml = HighlightConfiguration::new(
            tree_sitter_toml_ng::LANGUAGE.into(),
            "toml",
            tree_sitter_toml_ng::HIGHLIGHTS_QUERY,
            "",
            "",
        )
        .map_err(|error| error.to_string())?;
        toml.configure(HIGHLIGHT_NAMES);

        let mut markdown = HighlightConfiguration::new(
            tree_sitter_md::LANGUAGE.into(),
            "markdown",
            tree_sitter_md::HIGHLIGHT_QUERY_BLOCK,
            tree_sitter_md::INJECTION_QUERY_BLOCK,
            "",
        )
        .map_err(|error| error.to_string())?;
        markdown.configure(HIGHLIGHT_NAMES);

        let mut markdown_inline = HighlightConfiguration::new(
            tree_sitter_md::INLINE_LANGUAGE.into(),
            "markdown_inline",
            tree_sitter_md::HIGHLIGHT_QUERY_INLINE,
            tree_sitter_md::INJECTION_QUERY_INLINE,
            "",
        )
        .map_err(|error| error.to_string())?;
        markdown_inline.configure(HIGHLIGHT_NAMES);

        Ok(Self {
            rust,
            toml,
            markdown,
            markdown_inline,
        })
    }

    fn highlight(&self, language: LanguageId, buffer: &CodeBuffer) -> Option<HighlightedFile> {
        let config = self.config(language);
        let (source, metrics) = source_and_metrics(buffer);
        let mut lines = vec![HighlightedLine::default(); buffer.line_count()];
        let mut highlighter = Highlighter::new();
        let events = highlighter
            .highlight(config, source.as_bytes(), None, |name| {
                self.injection_config(name)
            })
            .ok()?;
        let mut stack = Vec::new();

        for event in events {
            match event.ok()? {
                HighlightEvent::HighlightStart(highlight) => {
                    stack.push(style_key_for(highlight));
                }
                HighlightEvent::HighlightEnd => {
                    let _ = stack.pop();
                }
                HighlightEvent::Source { start, end } => {
                    if let Some(style) = stack.last().copied() {
                        add_span_range(&mut lines, &metrics, start, end, style);
                    }
                }
            }
        }

        Some(HighlightedFile { lines })
    }

    fn config(&self, language: LanguageId) -> &HighlightConfiguration {
        match language {
            LanguageId::Rust => &self.rust,
            LanguageId::Toml => &self.toml,
            LanguageId::Markdown => &self.markdown,
        }
    }

    fn injection_config(&self, name: &str) -> Option<&HighlightConfiguration> {
        match name {
            "markdown_inline" => Some(&self.markdown_inline),
            "toml" => Some(&self.toml),
            _ => None,
        }
    }
}

fn source_and_metrics(buffer: &CodeBuffer) -> (String, Vec<LineMetrics>) {
    let mut source = String::new();
    let mut metrics = Vec::with_capacity(buffer.line_count());

    for (index, line) in buffer.lines().iter().enumerate() {
        let start_byte = source.len();
        source.push_str(line);
        let end_byte = source.len();
        metrics.push(LineMetrics {
            start_byte,
            end_byte,
        });

        if index + 1 < buffer.line_count() {
            source.push('\n');
        }
    }

    (source, metrics)
}

fn add_span_range(
    lines: &mut [HighlightedLine],
    metrics: &[LineMetrics],
    mut start: usize,
    end: usize,
    style: HighlightStyleKey,
) {
    if start >= end || metrics.is_empty() {
        return;
    }

    let mut line_index = line_index_for_offset(metrics, start);
    while start < end && line_index < metrics.len() {
        let line = metrics[line_index];
        if start > line.end_byte {
            line_index += 1;
            continue;
        }

        let segment_start = start.max(line.start_byte);
        let segment_end = end.min(line.end_byte);
        if segment_start < segment_end {
            push_line_span(
                &mut lines[line_index],
                segment_start - line.start_byte,
                segment_end - line.start_byte,
                style,
            );
        }

        if end <= line.end_byte {
            break;
        }

        start = line.end_byte.saturating_add(1);
        line_index += 1;
    }
}

fn line_index_for_offset(metrics: &[LineMetrics], offset: usize) -> usize {
    let partition_point = metrics.partition_point(|line| line.end_byte.saturating_add(1) <= offset);
    partition_point.min(metrics.len().saturating_sub(1))
}

fn push_line_span(
    line: &mut HighlightedLine,
    start_byte: usize,
    end_byte: usize,
    style: HighlightStyleKey,
) {
    if start_byte >= end_byte {
        return;
    }

    if let Some(last) = line.spans.last_mut()
        && last.style == style
        && last.end_byte == start_byte
    {
        last.end_byte = end_byte;
        return;
    }

    line.spans.push(HighlightSpan {
        start_byte,
        end_byte,
        style,
    });
}

fn style_key_for(highlight: Highlight) -> HighlightStyleKey {
    STYLE_KEYS
        .get(highlight.0)
        .copied()
        .unwrap_or(HighlightStyleKey::Variable)
}

#[cfg(test)]
mod tests {
    use super::{HighlightStyleKey, highlight_buffer};
    use crate::{CodeBuffer, syntax::LanguageId};

    #[test]
    fn highlights_rust_keywords_and_strings() {
        let highlighted = highlight_buffer(
            LanguageId::Rust,
            &CodeBuffer::from_text("fn main() {\n    let value = \"frame\";\n}\n"),
        )
        .expect("rust file should highlight");

        let first_line = highlighted.line(0).expect("first line exists");
        let second_line = highlighted.line(1).expect("second line exists");

        assert!(
            first_line
                .spans
                .iter()
                .any(|span| span.style == HighlightStyleKey::Keyword)
        );
        assert!(
            second_line
                .spans
                .iter()
                .any(|span| span.style == HighlightStyleKey::String)
        );
    }

    #[test]
    fn highlights_markdown_headings() {
        let highlighted = highlight_buffer(
            LanguageId::Markdown,
            &CodeBuffer::from_text("# Frame\nbody\n"),
        )
        .expect("markdown file should highlight");
        let first_line = highlighted.line(0).expect("first line exists");

        assert!(
            first_line
                .spans
                .iter()
                .any(|span| span.style == HighlightStyleKey::TextTitle)
        );
    }

    #[test]
    fn keeps_multiline_spans_scoped_to_each_line() {
        let highlighted = highlight_buffer(
            LanguageId::Rust,
            &CodeBuffer::from_text("let text = \"left\nright\";\n"),
        )
        .expect("rust file should highlight");

        assert!(
            highlighted
                .line(0)
                .expect("first line exists")
                .spans
                .iter()
                .any(|span| span.style == HighlightStyleKey::String)
        );
        assert!(
            highlighted
                .line(1)
                .expect("second line exists")
                .spans
                .iter()
                .any(|span| span.style == HighlightStyleKey::String)
        );
    }
}
