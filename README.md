# Frame

Frame is a terminal multiplexer focused on performance and extensibility, built around a
daemon-owned `Session -> Tab -> Pane` model.

Frame is under active development. Early releases are available for testing on Apple Silicon
macOS and ARM64/x86-64 Linux.

## Install and try

```sh
brew install phongndo/tap/frame
cd your-project
frame new --cwd "$PWD"
```

No compiler or Nix setup is needed. See [Installation](docs/installation.md) for the portable
installer, platform requirements, updates, and preview builds. See [Usage](docs/usage.md) for
sessions, panes, and key bindings.

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
