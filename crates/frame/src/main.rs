use std::process::ExitCode;

fn main() -> ExitCode {
    match frame_git::load_review_snapshot_from_current_dir() {
        Ok(snapshot) => {
            if let Err(error) = frame_view::run(snapshot) {
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
