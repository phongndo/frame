use std::{
    ffi::OsStr,
    io::Write,
    path::Path,
    process::{Command, Output, Stdio},
};

use super::GitError;

pub(crate) fn run_git<I, S>(cwd: &Path, args: I) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    run_git_allowing_status(cwd, args, &[0])
}

pub(crate) fn run_git_allowing_status<I, S>(
    cwd: &Path,
    args: I,
    allowed_statuses: &[i32],
) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let output = Command::new("git").current_dir(cwd).args(args).output();

    let output = match output {
        Ok(output) => output,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Err(GitError::GitUnavailable);
        }
        Err(error) => return Err(GitError::Io(error)),
    };

    handle_git_output(output, allowed_statuses)
}

pub(crate) fn run_git_with_input_allowing_status<I, S>(
    cwd: &Path,
    args: I,
    input: &[u8],
    allowed_statuses: &[i32],
) -> Result<Output, GitError>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    let child = Command::new("git")
        .current_dir(cwd)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn();

    let mut child = match child {
        Ok(child) => child,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Err(GitError::GitUnavailable);
        }
        Err(error) => return Err(GitError::Io(error)),
    };

    if let Some(mut stdin) = child.stdin.take() {
        stdin.write_all(input)?;
    }

    let output = child.wait_with_output()?;

    handle_git_output(output, allowed_statuses)
}

fn handle_git_output(output: Output, allowed_statuses: &[i32]) -> Result<Output, GitError> {
    if output
        .status
        .code()
        .is_some_and(|code| allowed_statuses.contains(&code))
    {
        return Ok(output);
    }

    let stderr = String::from_utf8_lossy(&output.stderr);
    if stderr.contains("not a git repository") {
        return Err(GitError::NotInRepo);
    }

    Err(GitError::CommandFailed(stderr.trim().to_owned()))
}
