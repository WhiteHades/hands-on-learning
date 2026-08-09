# Course Bundle Schema

Hands-on Learning installs provider-neutral course directories ending in
`.holcourse`. A bundle is complete, offline, versioned, and independent of the
account or API that produced its source material.

## Layout

```text
example.holcourse/
├── course.json
├── LICENSE
├── lessons/
├── files/
└── media/
```

Only `course.json` is fixed. Other directories are conventions. Every path in
the manifest is relative to the bundle root and uses `/` separators.

Hands-on Learning rejects absolute paths, empty path segments, `.` and `..`,
backslashes, control characters, symbolic links, hard links, devices, sockets,
and files not declared by the manifest.

## Manifest

Schema version 1 uses this shape:

```json
{
  "schema_version": 1,
  "id": "author.course-name",
  "version": "1.0.0",
  "minimum_app_version": "0.1.0",
  "title": "Course title",
  "description": "A short description.",
  "authors": ["Author name"],
  "language": "en",
  "tags": ["c", "beginner"],
  "license": {
    "spdx": "MIT",
    "file": "LICENSE",
    "attribution": "Required attribution text"
  },
  "files": [
    {
      "path": "LICENSE",
      "bytes": 1087,
      "sha256": "64-lowercase-hexadecimal-characters"
    }
  ],
  "chapters": []
}
```

IDs use lowercase ASCII letters, digits, dots, underscores, and hyphens. They
start with a letter or digit and contain at most 64 characters. Stable IDs let
course updates preserve learner progress.

`files` declares every regular file except `course.json` exactly once. The
loader verifies each byte count and SHA-256 digest before exposing the course.
It also walks the complete bundle to reject undeclared files.

## Chapters And Lessons

Each chapter has an `id`, `title`, and ordered `lessons` array. A lesson has:

```json
{
  "id": "print-greeting",
  "title": "Print a Greeting",
  "kind": "exercise",
  "content": "lessons/greeting.md",
  "workspace": null,
  "runner": null,
  "quiz": null,
  "media": []
}
```

`kind` is `reading`, `exercise`, or `quiz`. `content` points to UTF-8 Markdown.
`media` contains local paths to images, audio, or video files. Remote media URLs
do not belong in a normalized bundle.

### Workspaces

Exercise lessons declare starter files:

```json
{
  "default_file": "main.c",
  "files": [
    {
      "source": "files/main.c",
      "target": "main.c",
      "role": "editable",
      "syntax": "c"
    }
  ]
}
```

`source` is a bundle path. `target` is a relative path in the learner's private
workspace. Roles are `editable`, `readonly`, and `hidden`. Hidden files are
available to runners but do not appear in normal file navigation; they are not
secrets. Course refreshes copy only missing files and never overwrite learner
edits. Reset removes and recreates only the selected lesson workspace.

### Runners

Bundles select an application-owned runner and profile. They cannot provide
commands, executable paths, compiler flags, shell fragments, or environment
variables.

```json
{
  "id": "c",
  "profile": "c11",
  "check": {
    "kind": "stdout",
    "expected": "Hello, learner!\n"
  }
}
```

Supported C profiles are `c11`, `c11-32`, and `c23`. The `c11-32` profile is
for lessons that depend on 32-bit data layout and requires multilib support.
Check kinds are `stdout` and `tests`. A stdout check compares output after
removing trailing CR and LF characters. A test check passes when the supplied
test program exits successfully.

The application compiles with fixed argv arrays, uses a validated workspace,
captures standard output and error concurrently, limits output and resources,
starts a process group, and enforces a hard timeout. This reduces accidents but
is not a complete sandbox for hostile code.

### Quizzes

Quiz lessons contain one to 100 questions. Each question has two to ten choices
and names one choice ID as its answer:

```json
{
  "passing_score": 1,
  "questions": [
    {
      "id": "header",
      "prompt": "Which header declares printf?",
      "choices": [
        {"id": "a", "text": "math.h"},
        {"id": "b", "text": "stdio.h"}
      ],
      "answer": "b",
      "explanation": "stdio.h provides standard input/output declarations."
    }
  ]
}
```

## Catalog

`courses/catalog.json` lists freely downloadable bundles. Each entry contains
the stable course ID, version, title, description, license and attribution,
minimum application version, lesson count, HTTPS bundle URL, byte size, and
SHA-256 digest. The archive digest protects the distribution artifact; the
manifest inventory protects its extracted payload.

Catalog installation downloads to a temporary file, enforces a size limit,
verifies the archive digest, rejects unsafe archive entries, validates the
extracted course, and atomically moves it into the local course directory.

## Provider Imports

Provider extractors are private adapters. They may authenticate and transform
authorized source material, but their output must pass through this schema.
Normalization removes account data, source API URLs, source IDs, and remote
asset URLs. It retains license and attribution notices required by the content
grant.

An extractor must not publish a bundle until every included lesson, test,
solution, and asset has a redistribution license or written grant. The public
application never needs provider credentials and never calls a source API.
