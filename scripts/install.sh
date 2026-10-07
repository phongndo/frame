#!/bin/sh
set -eu

version=${FRAME_VERSION:-latest}
install_dir=${FRAME_INSTALL_DIR:-"$HOME/.local/bin"}
data_dir=${FRAME_DATA_DIR:-"$HOME/.local/share/frame"}

fail() { printf 'frame install: %s\n' "$*" >&2; exit 1; }
for tool in curl tar mktemp; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing required command: $tool"
done
if command -v sha256sum >/dev/null 2>&1; then
  checksum() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
  checksum() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  fail "shasum or sha256sum is required to verify the download"
fi

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) target=aarch64-apple-darwin ;;
  Linux-aarch64 | Linux-arm64) target=aarch64-unknown-linux-gnu ;;
  Linux-x86_64) target=x86_64-unknown-linux-gnu ;;
  *) fail "supported systems are Apple Silicon macOS and ARM64/x86_64 Linux" ;;
esac
case "$install_dir:$data_dir" in
  /*:/*) ;;
  *) fail "FRAME_INSTALL_DIR and FRAME_DATA_DIR must be absolute paths" ;;
esac

# Replace only a command owned by this installer. Homebrew and user commands
# remain under their existing owner's control.
destination="$install_dir/frame"
if [ -e "$destination" ] || [ -L "$destination" ]; then
  [ -L "$destination" ] || fail "$destination already exists; choose a different FRAME_INSTALL_DIR"
  case "$(readlink "$destination")" in
    "$data_dir"/releases/*/bin/frame) ;;
    *) fail "$destination is managed elsewhere; use its package manager or another FRAME_INSTALL_DIR" ;;
  esac
fi

if [ "$version" = latest ]; then
  metadata=$(curl -fsSL https://api.github.com/repos/phongndo/frame/releases/latest) || fail "no latest release is available; set FRAME_VERSION to a published tag"
  version=$(printf '%s\n' "$metadata" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
fi
case "$version" in v*) tag=$version ;; *) tag="v$version" ;; esac
printf '%s\n' "$tag" | LC_ALL=C grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$' || fail "invalid release version: $version"

package="frame-$tag-$target"
asset="$package.tar.gz"
base_url=${FRAME_RELEASE_BASE_URL:-"https://github.com/phongndo/frame/releases/download/$tag"}
temporary=$(mktemp -d)
pending=""
cleanup() {
  rm -rf "$temporary"
  if [ -n "$pending" ]; then rm -rf "$pending"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'Installing Frame %s for %s\n' "$tag" "$target"
curl -fSL --retry 3 "$base_url/$asset" -o "$temporary/$asset" || fail "could not download $asset"
curl -fsSL --retry 3 "$base_url/$asset.sha256" -o "$temporary/$asset.sha256" || fail "could not download the checksum"
expected=$(awk -v name="$asset" 'NF == 2 && $2 == name && length($1) == 64 && $1 !~ /[^0-9a-f]/ {print $1}' "$temporary/$asset.sha256")
if [ -z "$expected" ] || [ "$(checksum "$temporary/$asset")" != "$expected" ]; then
  fail "checksum verification failed"
fi
tar -xzf "$temporary/$asset" -C "$temporary"
for binary in frame frame-ui frame-config-host frame-clipboard-host; do
  [ -x "$temporary/$package/bin/$binary" ] || fail "archive is missing $binary"
done
[ -d "$temporary/$package/share/terminfo" ] || fail "archive is missing terminal definitions"

mkdir -p "$data_dir/releases" "$install_dir"
pending=$(mktemp -d "$data_dir/releases/$tag-$target.XXXXXX")
cp -R "$temporary/$package/." "$pending/"
"$pending/bin/frame" --version
ln -s "$pending/bin/frame" "$temporary/frame"
mv -f "$temporary/frame" "$destination"
pending=""

# shellcheck disable=SC2016 # Print a command for the reader's shell to expand.
printf '\nInstalled %s\nRun in a project: frame new --cwd "$PWD"\n' "$destination"
case ":$PATH:" in
  *":$install_dir:"*) ;;
  *) printf '\nAdd this directory to your shell PATH: %s\n' "$install_dir" ;;
esac
printf 'To update, rerun this installer after closing your Frame sessions.\n'
