# Hands-on Learning

Hands-on Learning is an offline, keyboard-first terminal course player written
in C23. It keeps lesson text, code, and local feedback together while handing
editing to your real Vim, Neovim, or LazyVim setup.

The application is version `0.1.0`.

## Features

- Native ncurses interface with adaptive wide and narrow layouts
- Vim and Neovim style navigation without function-key workflows
- Real `$VISUAL` or `$EDITOR` integration, with `nvim` as the default
- Catppuccin Mocha colors with exact RGB and deterministic 256-color fallback
- Generic, versioned, integrity-checked `.holcourse` directories
- Local C11, 32-bit C11, and C23 exercise profiles
- Exact output checks and supplied test program checks
- Bounded output, resource limits, hard timeouts, and process-group cleanup
- Reading, exercise, quiz, and local media lessons
- Course-scoped progress and workspace persistence with atomic writes
- A public catalog for freely downloadable courses
- A small original MIT-licensed demo course

The application itself is offline. Libcurl is used only by the explicit
catalog installation command.

## Requirements

- GCC with C23 support
- ncursesw
- json-c
- libcurl
- `bat` for syntax-aware previews, with a plain-text fallback
- Neovim, Vim, or another single-command terminal editor
- `mpv` for audio and video lessons
- `xdg-open` for image lessons
- GCC multilib only for courses that select the `c11-32` profile

Arch Linux:

```bash
sudo pacman -S base-devel ncurses json-c curl bat neovim mpv
```

Debian or Ubuntu:

```bash
sudo apt install build-essential libncurses-dev libjson-c-dev libcurl4-openssl-dev bat neovim mpv
```

## Build

```bash
make
make test
make sanitize
```

Run the bundled demo from the repository:

```bash
build/hands-on-learning
```

Install under `/usr/local`:

```bash
sudo make install
hands-on-learning
```

## Keybindings

`Space` is the leader key.

| Key | Action |
| --- | --- |
| `h`, `j`, `k`, `l` | Change pane or move in the focused view |
| `gg`, `G` | Move to the top or bottom |
| `Ctrl-d`, `Ctrl-u` | Move down or up half a page |
| `/` | Search the focused view or filter a picker |
| `n`, `N` | Move to the next or previous search match |
| `Esc` | Cancel a sequence, search, confirmation, or popup |
| `q` | Close a popup or quit from the root view |
| `Space l l` | Open the lesson picker |
| `Space l c` | Open the course picker |
| `Space r` | Compile and run the current exercise |
| `Space t` | Run the current check |
| `Space x` | Reset the current lesson after confirmation |
| `Space m` | Open local lesson media |
| `gt`, `gT`, `]b`, `[b` | Move through exercise files |
| `e`, Enter | Edit the current file or select an item |
| `?`, `Space ?` | Open the keybinding guide |

Arrow keys and the mouse wheel are secondary accessibility fallbacks. Pending
multi-key sequences expire after 750 milliseconds and `Esc` cancels them.

## Courses

Validate any installed directory:

```bash
hands-on-learning validate path/to/course.holcourse
```

Open one directly:

```bash
hands-on-learning --course path/to/course.holcourse
```

List the public catalog:

```bash
hands-on-learning catalog list
```

Install a free course:

```bash
hands-on-learning catalog install hol.demo-c
```

The public catalog is available at:

```text
https://raw.githubusercontent.com/WhiteHades/hands-on-learning/main/courses/catalog.json
```

The catalog verifies the downloaded archive size and SHA-256 digest. The
course loader then verifies every extracted payload file against the bundle
manifest and rejects undeclared files, unsafe paths, links, and special files.

See [`docs/course-schema.md`](docs/course-schema.md) to author a course or
write an authorized provider adapter. `.holcourse` and C symbols beginning
with `hol_` use `hol` only as an internal abbreviation for Hands-on Learning.

## Local Data

Hands-on Learning follows XDG directories:

- Progress: `$XDG_STATE_HOME/hands-on-learning/state.json`
- Workspaces: `$XDG_DATA_HOME/hands-on-learning/workspaces/`
- Installed courses: `$XDG_DATA_HOME/hands-on-learning/courses/`

If an XDG variable is unset, the application uses the corresponding directory
under `~/.local`.

Course refreshes copy only missing starter files. They do not overwrite learner
edits. Reset affects only the selected lesson and requires confirmation.

## Content Safety

This repository contains only original or explicitly licensed public content.
Raw provider archives, credentials, authenticated responses, account state,
learner state, and provider-specific extraction adapters are not part of the
public repository.

A normalized course can be published only when every lesson, test, solution,
and media asset has a free license or a written redistribution grant. Required
license and attribution notices remain in the course manifest and bundle.

## License

The application and original demo course are available under the MIT License.
Installed courses can use other licenses, which are displayed in their
manifests and catalog entries.
