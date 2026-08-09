# Issue tracker: GitHub

Issues and specifications for this repository live as GitHub issues. Use the
`gh` CLI for all operations.

## Conventions

- Create: `gh issue create --title "..." --body "..."`.
- Read: `gh issue view <number> --comments` and fetch labels.
- List: `gh issue list --state open --json number,title,body,labels,comments`.
- Comment: `gh issue comment <number> --body "..."`.
- Label: `gh issue edit <number> --add-label "..."` or `--remove-label "..."`.
- Close: `gh issue close <number> --comment "..."`.

Infer the repository from `git remote -v`. The `gh` CLI does this
automatically when run inside this checkout.

## Pull requests as a triage surface

PRs as a request surface: no.

GitHub shares one number space across issues and pull requests. Resolve an
ambiguous reference with `gh pr view <number>` and fall back to
`gh issue view <number>`.

## Skill operations

When a skill says to publish to the issue tracker, create a GitHub issue. When
a skill says to fetch the relevant ticket, run
`gh issue view <number> --comments`.

## Wayfinding

The map is one issue labelled `wayfinder:map`. Child tickets use GitHub
subissues when available and labels of the form `wayfinder:<type>`. Use native
GitHub issue dependencies for blocking edges. If those features are not
available, use task lists and a `Blocked by:` line. Claim a frontier ticket by
assigning it to the current user before making other writes. Resolve it with a
result comment, close it, then add its context pointer to the map.
