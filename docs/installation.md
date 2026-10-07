# Installation

Frame's early releases are intended for testing. Packages include the main executable, native
helpers, terminal definitions, and bundled extension examples. No compiler, Nix, or source checkout
is needed to run them.

Supported release targets are macOS 14 or newer on Apple Silicon, and glibc Linux on ARM64 or
x86-64 (tested on Ubuntu 24.04). Use the Nix package on NixOS. Python is only needed for optional
Python extensions; the default interface and configuration helpers are native executables.

| Method | Install | Update |
| --- | --- | --- |
| [Homebrew](#homebrew) | `brew install phongndo/tap/frame` | `brew upgrade frame` |
| [Portable installer](#portable-installer) | `curl -fsSL …/install.sh \| sh` | `frame update` |
| [mise](#mise) | `mise use -g github:phongndo/frame` | `mise upgrade github:phongndo/frame` |
| [Nix](#nix) | `nix profile install github:phongndo/frame` | `nix profile upgrade frame` |

Each method owns its installation; update Frame with the method that installed it. Close your Frame
Sessions before updating. The daemon exits after its final Session ends.

## Homebrew

```sh
brew install phongndo/tap/frame
frame new --cwd "$PWD"
```

The tap automatically picks up stable releases within six hours; dispatching its updater applies a
release immediately.

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

Update to the latest stable release, or select a version, including a prerelease:

```sh
frame update
frame update --version v0.1.0
```

`frame update` reruns the installer shipped with the running release, using the directories it was
installed with. It does nothing when the requested release is already installed, and refuses to
manage Homebrew, Nix, mise, or source installations. Old installation directories are retained so
an update cannot remove a running process's helpers; unused directories can be removed after all
Sessions have closed.

The installer accepts the same choice when piped:

```sh
curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh |
  FRAME_VERSION=v0.1.0 sh
```

`FRAME_INSTALL_DIR` changes the command directory and `FRAME_DATA_DIR` changes the release directory;
both must be absolute paths. The installer refuses to replace an unrelated command or a Homebrew
installation. `FRAME_RELEASE_BASE_URL` can point to a download mirror containing a selected release's
archives and checksum files.

### Nightly builds

The [Nightly workflow](https://github.com/phongndo/frame/actions/workflows/nightly.yml) republishes
the archives of the latest tested `main` commit daily as the `vnightly` prerelease. Expect breaking
changes. Switch to it, and back to stable, with:

```sh
frame update --version nightly
frame update
```

For a new installation, set `FRAME_VERSION=nightly` on the installer. Each nightly update installs a
fresh copy, even when `main` is unchanged.

## mise

[mise](https://mise.jdx.dev/) installs release archives directly from GitHub, verifying their
checksums:

```sh
mise use -g github:phongndo/frame
frame new --cwd "$PWD"
```

Pin a version with `github:phongndo/frame@0.1.0`, including in a project's `mise.toml`.

## Nix

Nix builds Frame from source with its pinned dependencies; on NixOS, this is the supported method.
Try it without installing:

```sh
nix run github:phongndo/frame -- new --cwd "$PWD"
```

Install it into your user profile:

```sh
nix profile install github:phongndo/frame
```

For declarative configuration, add the flake as an input and its package to your system or
Home Manager packages:

```nix
{
  inputs.frame.url = "github:phongndo/frame";

  # In a nix-darwin or NixOS module:
  environment.systemPackages = [ inputs.frame.packages.${pkgs.stdenv.hostPlatform.system}.default ];
  # Or in Home Manager:
  home.packages = [ inputs.frame.packages.${pkgs.stdenv.hostPlatform.system}.default ];
}
```

Update with `nix flake update frame` and rebuild. From a source checkout, `nix build .#frame` builds
the same package into `./result`, and `nix profile install .` installs it. The portable Linux
archives target conventional glibc distributions; the Nix package carries its runtime closure.

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
provide persistent downloads. The same layout is attached to every
[GitHub release](https://github.com/phongndo/frame/releases).

## Removal

Close Frame Sessions first, then use the installing method:

- Homebrew: `brew uninstall frame`
- Portable installer: remove `~/.local/bin/frame` and `~/.local/share/frame`, or the directories
  selected by `FRAME_INSTALL_DIR` and `FRAME_DATA_DIR`
- mise: `mise uninstall github:phongndo/frame` and remove it from your mise configuration
- Nix: `nix profile remove frame`, or remove the flake input and package

Your [configuration](configuration.md) is separate and is retained.
