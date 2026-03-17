#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeBuffer {
    lines: Vec<String>,
}

impl CodeBuffer {
    #[must_use]
    pub fn from_text(text: &str) -> Self {
        let lines = if text.is_empty() {
            vec![String::new()]
        } else {
            text.lines().map(ToOwned::to_owned).collect()
        };

        Self { lines }
    }

    #[must_use]
    pub fn placeholder(message: impl Into<String>) -> Self {
        Self {
            lines: vec![message.into()],
        }
    }

    #[must_use]
    pub fn line(&self, index: usize) -> Option<&str> {
        self.lines.get(index).map(String::as_str)
    }

    #[must_use]
    pub fn lines(&self) -> &[String] {
        &self.lines
    }

    #[must_use]
    pub fn line_count(&self) -> usize {
        self.lines.len()
    }

    #[must_use]
    pub fn to_source(&self) -> String {
        self.lines.join("\n")
    }
}
