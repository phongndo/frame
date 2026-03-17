use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LanguageId {
    Rust,
    Toml,
    Markdown,
}

impl LanguageId {
    #[must_use]
    pub fn detect(path: &str) -> Option<Self> {
        let extension = Path::new(path).extension()?.to_str()?.to_ascii_lowercase();

        match extension.as_str() {
            "rs" => Some(Self::Rust),
            "toml" => Some(Self::Toml),
            "md" | "markdown" => Some(Self::Markdown),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::LanguageId;

    #[test]
    fn detects_supported_languages_from_extensions() {
        assert_eq!(LanguageId::detect("src/main.rs"), Some(LanguageId::Rust));
        assert_eq!(LanguageId::detect("Cargo.toml"), Some(LanguageId::Toml));
        assert_eq!(LanguageId::detect("README.md"), Some(LanguageId::Markdown));
        assert_eq!(
            LanguageId::detect("docs/guide.markdown"),
            Some(LanguageId::Markdown)
        );
    }

    #[test]
    fn ignores_unsupported_paths() {
        assert_eq!(LanguageId::detect("Cargo.lock"), None);
        assert_eq!(LanguageId::detect("src/main"), None);
        assert_eq!(LanguageId::detect(""), None);
    }
}
