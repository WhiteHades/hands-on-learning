# Use the real terminal editor

Hands-on Learning keeps reading, preview, navigation, and feedback in ncurses, but it does not embed a text editor. Editing suspends ncurses and opens the learner's configured Vim or Neovim process because this preserves the learner's real configuration, plugins, motions, and accessibility setup. An embedded editor would make the application larger while still providing a weaker Vim experience.
