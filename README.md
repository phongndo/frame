# Frame

Frame is a terminal multiplexer focused on performance and extensibility, built around a
daemon-owned `Session -> Tab -> Pane` model.

Frame is under active development. Early releases are available for testing on Apple Silicon
macOS and ARM64/x86-64 Linux.

## Install and try

Homebrew on macOS or Linux:

```sh
brew install phongndo/tap/frame
```

Or use the portable installer, [mise](https://mise.jdx.dev/), or Nix:

```sh
curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh | sh
mise use -g github:phongndo/frame
nix profile install github:phongndo/frame
```

Then start a Session in your project:

```sh
cd your-project
frame new --cwd "$PWD"
```

Update with the method that installed Frame; portable installations use `frame update`. See
[Installation](docs/installation.md) for platform requirements, nightly builds, and removal, and
[Usage](docs/usage.md) for sessions, panes, and key bindings.

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
