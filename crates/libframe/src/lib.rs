#![doc = r"
`libframe` will eventually host reusable diff and review primitives.

The first scaffold commit keeps the public API intentionally small: one shared
status string used by the CLI to prove the crate boundary and workspace wiring.
"]

/// Shared placeholder status line for the scaffold binary.
pub const SCAFFOLD_STATUS_LINE: &str = "frame scaffold: review UI not implemented yet";

/// Returns the current scaffold status line for the CLI.
#[must_use]
pub const fn scaffold_status_line() -> &'static str {
    SCAFFOLD_STATUS_LINE
}

#[cfg(test)]
mod tests {
    use super::scaffold_status_line;

    #[test]
    fn scaffold_status_mentions_review_ui() {
        assert!(scaffold_status_line().contains("review UI"));
    }
}
