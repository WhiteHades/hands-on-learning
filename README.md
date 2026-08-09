# Hands-on Learning

Study the Boot.dev C and SQL courses in one terminal app.

Hands-on Learning uses Vim keybindings and stores your progress locally.

## Get started

On Ubuntu, install the required libraries:

```bash
sudo apt install build-essential bubblewrap util-linux sqlite3 libncurses-dev libjson-c-dev libcurl4-openssl-dev libxml2-dev libarchive-dev zip
```

Build and start the app:

```bash
make
build/hands-on-learning
```

Choose the C or SQL course from the menu. The app downloads it once and opens it. Later launches reuse the downloaded copy.

## Use the app

- Press `Space l l` to choose a lesson
- Press `j` and `k` to move
- Press `/` to search
- Press `Enter` to answer a quiz
- Press `?` to see all keybindings
- Press `q` to close a popup or quit

## Install the command

To run the app as `hands-on-learning` from any directory:

```bash
sudo make install
hands-on-learning
```

## Open another course

The app also reads local IMS Common Cartridge 1.3 packages:

```bash
build/hands-on-learning --course path/to/course.imscc
```

Local cartridges stay content-only unless you explicitly associate an exercise profile:

```bash
build/hands-on-learning --course path/to/course.imscc --exercise-profile path/to/course.profile.json
```

Local checks integrity-protect packaged tests and expected output against accidental edits. They are not an anti-cheat boundary against deliberately hostile learner code.

The included C and SQL courses show Boot.dev as their source and author.

See [`docs/course-schema.md`](docs/course-schema.md) for the supported standards profile.
