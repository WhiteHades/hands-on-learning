# Hands-on Learning

Learn by reading, editing, running, and checking code in one terminal interface.

Hands-on Learning is a native C23 application. It uses ncurses, Vim keybindings, your real editor, and offline course bundles.

Version: `0.1.0`

## Build and start

Install GCC, ncursesw, json-c, libcurl, `bat`, and Neovim.

```bash
make check
build/hands-on-learning
```

The included course teaches the main workflow in three lessons.

## Use your editor

Press `e` or Enter on an editable file. The application pauses ncurses and opens `$VISUAL`, then `$EDITOR`, then `nvim`.

Close the editor to return to the same lesson and scroll position.

## Navigate

`Space` is the leader key.

| Key | Action |
| --- | --- |
| `h` `j` `k` `l` | Change pane or move |
| `gg` `G` | Go to the top or bottom |
| `Ctrl-d` `Ctrl-u` | Move half a page |
| `/` `n` `N` | Search and move between matches |
| `Space l l` | Open the lesson picker |
| `Space l c` | Open the course picker |
| `Space r` | Run the exercise |
| `Space t` | Check the answer |
| `Space x` | Reset the current lesson |
| `Space m` | Open local lesson media |
| `gt` `gT` `]b` `[b` | Change the code file |
| `?` | Show all keys |
| `q` | Close a popup or quit |

## Add courses

List the public catalog:

```bash
build/hands-on-learning catalog list
```

Install a course:

```bash
build/hands-on-learning catalog install hol.demo-c
```

Open a local bundle:

```bash
build/hands-on-learning --course path/to/course.holcourse
```

Read [`docs/course-schema.md`](docs/course-schema.md) to create a `.holcourse` bundle.

## Verify content

Each catalog entry records the archive size and SHA-256 digest. Each course manifest records every payload file, size, and digest.

```bash
build/hands-on-learning validate path/to/course.holcourse
```

The loader rejects bad digests, missing files, undeclared files, unsafe paths, links, and special files.

## Course attribution

Imported courses identify Boot.dev as their source. Their manifests, catalog entries, and course views show the required Boot.dev attribution.

The included demo is original Hands-on Learning content. It does not contain Boot.dev lessons or media.
