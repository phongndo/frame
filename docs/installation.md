# Installation

Frame's early releases are intended for testing. Packages include the main executable, native
helpers, terminal definitions, and bundled extension examples. No compiler, Nix, or source checkout
is needed to run them.

Supported release targets are macOS 14 or newer on Apple Silicon, and glibc Linux on ARM64 or
x86-64 (tested on Ubuntu 24.04). Use the Nix package on NixOS. Python is only needed for optional
Python extensions; the default interface and configuration helpers are native executables.

## Homebrew

```sh
brew install phongndo/tap/frame
frame new --cwd "$PWD"
```

To update, close your Frame Sessions, then run `brew upgrade frame`. The tap automatically picks up
stable releases within six hours; dispatching its updater applies a release immediately.

## Portable installer

```sh
curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh | sh
```

The installer requires `curl`, `tar`, and `shasum` or `sha256sum`. It verifies the archive's SHA-256,
installs a complete versioned directory under `~/.local/share/frame/releases`, and links the command
at `~/.local/bin/frame`. Add that directory to your shell's PATH if needed:

```sh
export PATH="$HOME/.local/bin:$PATH"
frame new --cwd "$PWD"
```

Close your Frame Sessions and rerun the installer to update. The daemon exits after its final
Session ends. Old installation directories are retained so an update cannot remove a running
process's helpers; unused directories can be removed after all Sessions have closed.

To select a version, including a prerelease:

```sh
curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh |
  FRAME_VERSION=v0.1.0 sh
```

`FRAME_INSTALL_DIR` changes the command directory and `FRAME_DATA_DIR` changes the release directory;
both must be absolute paths. The installer refuses to replace an unrelated command or a Homebrew
installation. `FRAME_RELEASE_BASE_URL` can point to a download mirror containing a selected release's
archives and checksum files.

## Nix

```sh
nix run github:phongndo/frame -- new --cwd "$PWD"
```

For a persistent installation, add Frame's flake package to your Nix configuration. The portable
Linux archives target conventional glibc distributions; the Nix package carries its runtime closure.

## Preview builds

The [Distribution workflow](https://github.com/phongndo/frame/actions/workflows/distribution.yml)
uploads a tested archive for each supported platform. Download the matching artifact, unzip it,
verify its checksum, and extract the archive:

```sh
shasum -a 256 -c frame-v0.1.0-aarch64-apple-darwin.tar.gz.sha256
tar -xzf frame-v0.1.0-aarch64-apple-darwin.tar.gz
./frame-v0.1.0-aarch64-apple-darwin/bin/frame new --cwd "$PWD"
```

Use the version and target named in your download; `sha256sum -c` also works on Linux. Keep the
extracted directory together: the main executable needs its sibling helpers and `share/terminfo`.
Its `bin/frame` can be symlinked onto PATH. CI artifacts expire after 14 days; published releases
provide persistent downloads.

## Removal

Use `brew uninstall frame` for Homebrew. For a default portable installation, close Frame Sessions,
then remove `~/.local/bin/frame` and `~/.local/share/frame`. Your
[configuration](configuration.md) is separate and is retained.
