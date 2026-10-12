# Frame

Frame is a terminal multiplexer focused on performance and extensibility, built around a
daemon-owned `Session -> Tab -> Pane` model.

Frame is under active development; early releases are intended for testing.

## Install

Frame runs on Apple Silicon macOS 14 or newer and on ARM64 and x86-64 Linux.

### Homebrew

```sh
brew install phongndo/tap/frame
```

### Install script

```sh
curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh | sh
```

### mise

```sh
mise use -g github:phongndo/frame
```

### Nix

```sh
nix profile add github:phongndo/frame
```

Update with `frame update`, however you installed it. [Installation](docs/installation.md) covers
platform details, pinning a version, nightly builds, and removal.

## Start

In your project directory, start a Session:

```sh
frame new --cwd "$PWD"
```

[Usage](docs/usage.md) covers sessions, panes, key bindings, and building from source.

## Documentation

- [Usage](docs/usage.md): build, run, and operate Frame
- [Installation](docs/installation.md): install, update, and try releases
- [Configuration](docs/configuration.md): Lua settings, keymaps, and custom commands
- [Automation API](docs/api.md): execute Procs and observe Events
- [Runtime extensions](docs/extensions.md): connect external programs and present Surfaces
- [Architecture](docs/architecture.md): component boundaries, ownership, and data flow
- [Development](docs/development.md): contributor workflow and verification
- [Performance](docs/performance.md): measurement and review requirements

## License

[MIT](LICENSE)
