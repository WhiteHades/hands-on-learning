# Hands-on Learning

Learn by reading, editing, running, and checking code in one terminal app.

Hands-on Learning is written in C23. It uses Vim keybindings and opens your real Vim, Neovim, or LazyVim setup for editing.

## Get started

Install GCC, ncursesw, json-c, libcurl, `bat`, and Neovim. Then run:

```bash
make check
build/hands-on-learning
```

The app starts with a three lesson demo course.

## Use the app

- Press `Space l l` to choose a lesson
- Press `e` to edit the current file
- Press `Space r` to run your code
- Press `Space t` to check your answer
- Press `?` to see all keybindings
- Press `q` to close a popup or quit

Your lesson stays open when Neovim starts. Close Neovim to return to the same place.

## Add a course

List the free courses:

```bash
build/hands-on-learning catalog list
```

Install a course:

```bash
build/hands-on-learning catalog install hol.demo-c
```

Open any local course bundle:

```bash
build/hands-on-learning --course path/to/course.holcourse
```

Imported courses show Boot.dev as their source and author. The included demo is original Hands-on Learning content.

See [`docs/course-schema.md`](docs/course-schema.md) to create a course bundle.
