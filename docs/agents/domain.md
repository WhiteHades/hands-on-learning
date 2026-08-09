# Domain Docs

This is a single-context repository. Engineering skills use the following
rules when they explore or change the codebase.

## Read first

- Read root `CONTEXT.md` when it exists.
- Read records in `docs/adr/` that affect the area under change.

If these files do not exist, proceed silently. Domain modelling skills create
them when terminology or decisions are actually resolved.

## Layout

```text
/
├── CONTEXT.md
├── docs/adr/
└── src/
```

## Vocabulary

Use terms exactly as the glossary in `CONTEXT.md` defines them. Do not drift to
synonyms that the glossary rejects. If a needed concept is absent, first check
whether the proposed term duplicates an existing concept. Record a genuine
domain gap for the domain modelling workflow.

## Decisions

Surface conflicts with an architecture decision record. Do not silently
override a recorded decision. State which ADR conflicts and why reopening the
decision could be justified.
