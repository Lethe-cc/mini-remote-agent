# Mini Remote Client

A small C++20 desktop SSH terminal and SFTP client. Dear ImGui provides the UI,
GLFW/OpenGL provide the window, libssh2 handles SSH/SFTP, and libvterm interprets
terminal escape sequences. macOS is the build target; the CMake dependency
handling is portable, but only macOS is verified.

## Features

- Password or private-key authentication, including encrypted private keys.
- Interactive PTY shell with ANSI colors, alternate screen, UTF-8, resize,
  function/control keys, selection, clipboard and 5,000 lines of scrollback.
  A CJK font is merged into the terminal face so Chinese output is not lost.
- Local/remote directory browsers, single-file uploads/downloads, rename,
  folder creation and deletion of files or empty directories.
- Background local directory scans with latest-request handling; failed scans
  retain the previous directory and listing.
- Sequential transfer queue with progress, average throughput and cancellation.
- Disconnecting, by button or by a dropped session, clears the entered host and
  username, empties the terminal screen, dismisses the connection dialog and
  rewrites the saved profile, so none of it is restored on the next start: no
  manual cleanup, no typed input.
- First-use host fingerprint confirmation and persistent OpenSSH known-host
  records. A changed key is rejected before authentication.
- Separate SSH and SFTP worker threads/connections; network work is not done
  on the UI thread. Both connections independently verify the host key.
- Temporary-file transfers: cancellation does not truncate the destination.
  Existing destinations require explicit confirmation.

## Build With CMake

Requirements: CMake 3.22+, Ninja, Perl, an Apple Clang/Xcode toolchain with
C++20 `std::jthread` support, and OpenGL 3.2 (supplied by macOS). Homebrew
supplies the optional system libraries:

```sh
brew install cmake ninja glfw libssh2 libvterm
```

Every third-party dependency is resolved by CMake itself; there is no package
manager step. Each module looks for an installed library first and otherwise
builds the pinned upstream tarball with `FetchContent`, verified against a
SHA-256 digest:

| Dependency | System search | Pinned bundled version | Switch |
| --- | --- | --- | --- |
| GLFW | `find_package(glfw3 CONFIG)` | 3.5.1 | `MRC_BUNDLED_GLFW` |
| Dear ImGui | `find_package(imgui CONFIG)` | 1.92.9 | `MRC_BUNDLED_IMGUI` |
| libssh2 | `find_package(libssh2 CONFIG)`, then header + library | 1.11.1 | `MRC_BUNDLED_SSH2` |
| libvterm | header + library search | 0.3.3 (neovim commit) | `MRC_BUNDLED_VTERM` |

Homebrew has no `imgui` formula, so Dear ImGui — and its GLFW and OpenGL3
backends, which upstream does not package for installation — is normally built
from source. Setting a `MRC_BUNDLED_*` switch to `ON` forces that source build
even when a system copy exists.

```sh
cmake --preset system          # brew GLFW, libssh2 and libvterm; ImGui from source
cmake --build --preset system
ctest --preset system
./build/system/mini-remote-client
```

Three presets are provided: `system` (Debug), `release` (Release, same
dependency search) and `bundled` (Release, every dependency built from the
pinned sources with `MRC_BUNDLED_*=ON`). The `bundled` build needs Homebrew
OpenSSL (`brew install openssl@3`) as libssh2's crypto backend and links
GLFW, Dear ImGui, libssh2 and libvterm statically, so the executable depends
only on the OpenSSL dylib and system frameworks.

### Offline builds

`MRC_LOCAL_DEPS_DIR` defaults to `build-deps/`. If it contains
`<name>-<version>.tar.gz` and that file's digest matches the pinned one, the
tarball is used instead of the network; the checkout already ships
`imgui-1.92.9.tar.gz`, `libssh2-1.11.1.tar.gz` and `libvterm-0.3.3.tar.gz`.
Only GLFW 3.5.1 is then downloaded. Alternatively pre-extract a dependency and
pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=/path/to/source`, or set
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON` once everything is populated. To raise
a pinned version, update both `MRC_<DEP>_VERSION` and `MRC_<DEP>_SHA256`.

libssh2 1.11.0 supports normal transfers, but remote overwrite is intentionally
refused. With 1.11.1+, overwrite uses the server's `posix-rename@openssh.com`
extension; if unsupported, the original is retained and the operation fails.
There is no delete-before-rename fallback. Configuring prints the detected
version and warns when it is older than `MRC_SSH2_MINIMUM`.

Building the bundled libssh2 prints one deprecation notice from libssh2's own
`cmake_minimum_required(VERSION 3.7)`; it comes from that project, not this one.

For distribution, include the runtime libraries and third-party license files;
this project does not yet produce signed application bundles or installers, and
the executable runs directly from the build directory.

## Current Workspace Build

The verified macOS (Apple Silicon, Apple Clang 21, CMake 4.4) executables in
this workspace are `build/system/mini-remote-client` and
`build/bundled/mini-remote-client`; both pass `ctest --preset <name>`. Build
directories and downloaded sources under `build/` are not source-controlled.

## Usage

1. Open Connection, enter the host, port, user and authentication details.
2. Verify a new host's SHA-256 fingerprint independently before trusting it.
   The displayed fingerprint uses hexadecimal bytes, explicitly labeled as
   such, not OpenSSH's default base64 display.
3. Click inside Terminal to focus keyboard input. Drag to select text;
   Ctrl+Shift+C copies the selection (or visible screen), Ctrl+Shift+V pastes.
   Cmd+C/Cmd+V also work on macOS. Mouse wheel or Shift+PageUp/PageDown accesses
   scrollback; Latest returns to the live screen.
4. In Files, double-click a directory or enter a path and press Enter. Select
   a regular file and use the arrow toward the destination pane to transfer.
   Rename/Delete operate on the selected item. Deletion is permanent and
   requires confirmation; nonempty directories are not recursively removed.
5. Use Disconnect before changing connections. Disconnecting - whether you press
   the button or the server or network drops the session - clears the entered
   host and username, empties the terminal screen and its scrollback, closes the
   connection dialog by itself and drops the identity from
   the saved profile, so nothing has to be selected, typed or re-entered after a
   restart. Authentication secrets are
   cleared from application buffers after use and are never saved in profiles.

Configuration is stored in `$XDG_CONFIG_HOME/mini-remote-client`, or
`~/.config/mini-remote-client` when unset; Windows uses
`%APPDATA%/mini-remote-client`. The port, authentication mode and key path are
saved in `connection.txt`, and trusted keys in `known_hosts`. The host and
username are removed from that file whenever a session closes, so they are never
restored after a disconnect. No password or passphrase
is stored. Only change a saved key after independently verifying the server.

The app draws the terminal with a system monospace font and then merges extra
faces into it, because no single system face covers a shell prompt. Merged faces
only fill glyphs the primary font lacks, so Latin metrics stay those of the
primary font:

| Tier | Provides | Candidates |
| --- | --- | --- |
| primary | Latin, box drawing, `❯` `✔` | `MRC_FONT`, DejaVu Sans Mono, Liberation Mono, Menlo, Consolas |
| CJK | Han ideographs (中 国) | `MRC_CJK_FONT`, Noto Sans/Serif CJK, WenQuanYi, PingFang, Hiragino Sans GB, STHeiti, Songti, Arial Unicode, msyh |
| icons | private-use-area icons used by starship and powerlevel10k (`` `` ``) | `MRC_ICON_FONT`, else a Nerd Font or Powerline font found in `~/Library/Fonts`, `~/.local/share/fonts` or `/Library/Fonts` |
| symbols | remaining symbol blocks (`⬆` `⚡` and a monochrome `🌐`) | Apple Symbols, DejaVu Sans, Segoe UI Emoji |

Every candidate is checked for existence first, because ImGui asserts on a
missing font file. `MRC_ICON_FONT=none` disables icon-font detection. Glyph
availability comes from these merged faces: Dear ImGui 1.92 bakes glyphs lazily,
so the ranges requested at load time (`GetGlyphRangesChineseFull` plus the arrow,
technical, box-drawing, geometric, symbol, dingbat and private-use-area blocks)
are a preloading hint rather than a filter. Colour emoji are not supported -
ImGui rasterises monochrome outlines only, so Apple Color Emoji cannot be used;
`🌐` renders from Apple Symbols instead. Not every symbol resolves on every
platform: `⏱` (U+23F1) has no glyph in any macOS system font measured here.
Emoji and complex shaping are not guaranteed.

## Verification

`terminal_test` exercises colors, alternate screen, UTF-8/wide cells, key
encoding, bracketed paste, resizing and scrollback, and `app_test` additionally
loads the real terminal font and asserts it covers 中国 plus box-drawing and Greek
cells, that the requested ranges include the private-use area and the symbol
blocks that starship and powerlevel10k prompts use, and - where a Nerd Font or
Powerline font is installed - that merging it supplies the powerline branch icon
without losing the primary monospace face. `app_test` directly injects
ImGui mouse events to verify file-tab activation and small-window rendering;
passing an output PPM path additionally renders a screenshot with OpenGL.
It also checks background local scans, directory ordering, invalid paths and
superseded requests, and that a closed session clears the entered host and
username, empties the terminal screen, dismisses the connection dialog and keeps
the identity cleared: `app_test` points `XDG_CONFIG_HOME` at a temporary profile,
connects to a port where nothing listens, waits for both workers to report the
closure, then asserts the fields are empty, the screen holds no printable
c content, no popup remains open, and a freshly constructed `App` reading
the same profile still shows no host or username. `terminal_test` already covers
the clearing primitive itself (`reset` drops screen content and scrollback).
Optional integration tests start an
isolated unprivileged OpenSSH server on a temporary loopback port, using
temporary host/client keys and files:

```sh
cmake --preset system -DMRC_SSH_INTEGRATION_TESTS=ON
cmake --build --preset system
ctest --preset system
```

This needs Python 3, `sshd`, `ssh-keygen`, and a local user that OpenSSH allows
to log in. It never changes `/etc/ssh` or the user's authorized keys. Tests
cover shell I/O, trust persistence, changed-key rejection, binary/empty file
round trips, overwrite confirmation, rename/delete, special-file rejection,
source revalidation, upload/download cancellation and disconnect cleanup.
These tests cannot run unprivileged on macOS: the system `sshd` authenticates the
temporary key and starts the channel, but then refuses to set up the session as a
non-root user (`Attempt to write login records by non-root user (aborting)`, plus
`setaudit_addr failed: Operation not permitted`), so the PTY never opens and
`remote_test` times out waiting for shell output. The CI workflow therefore leaves
`MRC_SSH_INTEGRATION_TESTS` off on macOS runners.
`--smoke-test` renders five GUI
frames and exits; it needs a window server session, so run it from a logged-in
desktop rather than a headless SSH session.

macOS builds (Apple Silicon, `system`, `release` and `bundled` presets) were
executed in the development environment, with `terminal_test`, `app_test` and
`--smoke-test` passing. Default and 640x480
window layouts were checked.

CMake exports `compile_commands.json` in the selected build directory. `.clangd`
and `.vscode/settings.json` point clangd at `build/system`; change both if you
configure elsewhere. Note that a build directory records the path spelling CMake
was invoked with - here `/Users/marvin/workspace` is a symlink to
`/Volumes/workspace` - and FetchContent keeps the original spelling after a
reconfigure, so mixing the two leaves `-I` paths clangd cannot match, which shows
up as a cascade of false errors (`std::jthread`, `std::filesystem`, `ImFont`).
Always run CMake, the editor and clangd from one path, and allow the actual
compiler with `--query-driver=/usr/bin/c++` if standard C++ headers are not
detected. Do not fix language-server discovery errors by hardcoding
machine-specific standard-library paths in the source.

## Scope and Limitations

- One saved connection profile and one active terminal; no SSH agent, jump
  hosts, keyboard-interactive/2FA, session tabs or automatic reconnect yet.
- File transfers are single regular files, not recursive directories,
  symlinks, synchronization or resumable transfers. Uploads use mode 0600 and
  do not preserve source permissions/timestamps. Sources are rechecked when
  jobs execute, and remote download handles are checked before reading.
  Files with unknown remote types are rejected rather than assumed regular.
- Local no-overwrite downloads publish via a hard link to avoid a race with
  newly created destinations. Filesystems without hard-link support (such as
  FAT) fail safely instead of overwriting. Temporary files are cleaned on
  handled errors/cancellation, but may remain after a crash or lost connection.
- SSH operations time out after eight seconds; cancellation can take that
  long during a blocked request. OS hostname resolution is synchronous inside
  a worker and may take longer. Local directory resolution, scanning and sorting
  run on a separate worker; only the latest request updates the UI. OS filesystem
  calls cannot be interrupted and can delay shutdown on unresponsive mounts.
  Local create/rename/delete operations still use the UI thread.
- OSC clipboard commands, mouse-reporting terminal applications, and complete
  terminal conformance are outside this first version. Common full-screen
  tools use libvterm's alternate screen and key modes, not a custom ANSI parser.
- Passwords are not persisted, but this is not a hardened secret-storage
  system: OS swap, clipboard, third-party buffers and process dumps are not
  controlled. Use a supported modern SSH library and verify fingerprints.

## Source Layout

- `src/terminal.*`: libvterm screen, input encoding and bounded scrollback.
- `src/remote.*`: worker ownership, socket connections, trust and file jobs.
- `src/app.*`: ImGui views, confirmations, profile, background local directory
  scanning and event handling.
- `src/main.cpp`: GLFW/OpenGL lifetime, fonts and styling.
- `cmake/{Glfw,Imgui,Ssh2,VTerm}.cmake`: system search plus pinned FetchContent
  builds; `cmake/PinnedSource.cmake` holds the shared download, digest and
  subproject-option helpers.
- `tests/`: terminal checks and isolated SSH/SFTP integration fixture.