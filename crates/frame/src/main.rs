use std::process::ExitCode;

fn main() -> ExitCode {
    match libframe::load_diff_from_current_dir() {
        Ok(repo_diff) => {
            if let Err(error) = frame_view::run(repo_diff.diff) {
                eprintln!("frame: {error}");
                return ExitCode::FAILURE;
            }
        }
        Err(error) => {
            eprintln!("frame: {error}");
            return ExitCode::FAILURE;
        }
    }

    ExitCode::SUCCESS
}
