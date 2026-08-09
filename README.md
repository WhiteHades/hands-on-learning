# Hands-on Learning

Read portable courses and take quizzes in one terminal app.

Hands-on Learning is a C23 reader for 1EdTech Common Cartridge 1.3 packages. It uses Vim keybindings and stores progress locally.

## Get started

Install GCC, ncursesw, json-c, libcurl, libxml2, libarchive, and zip. Then run:

```bash
make check
build/hands-on-learning
```

The app starts with a three lesson IMS Common Cartridge demo.

## Use the app

- Press `Space l l` to choose a lesson
- Press `j` and `k` to move
- Press `/` to search
- Press `Enter` to answer a quiz
- Press `?` to see all keybindings
- Press `q` to close a popup or quit

## Add a course

List the free courses:

```bash
build/hands-on-learning catalog list
```

Install a course:

```bash
build/hands-on-learning catalog install bootdev.learn-sql
```

Open any local Common Cartridge package:

```bash
build/hands-on-learning --course path/to/course.imscc
```

Imported courses show Boot.dev as their source and author. The included demo is original Hands-on Learning content.

See [`docs/course-schema.md`](docs/course-schema.md) for the supported standards profile.
