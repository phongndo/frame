mod highlight;
mod language;

pub use highlight::{
    HighlightSpan, HighlightStyleKey, HighlightedFile, HighlightedLine, highlight_buffer,
};
pub use language::LanguageId;
