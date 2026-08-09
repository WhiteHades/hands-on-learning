# ADR 0002: Use IMS Common Cartridge

## Status

Superseded by [ADR 0003](0003-use-integrity-bound-exercise-profiles.md).

## Context

Hands-on Learning needs course packages that established learning platforms can exchange. A project-specific directory and manifest would make the application the only complete reader.

1EdTech Common Cartridge 1.3 standardizes ZIP packaging, organization structure, learning resources, LOM metadata, and QTI 1.2.1 assessments.

Common Cartridge does not standardize native terminal workspaces, compiler profiles, SQL runners, shell commands, or expected process output.

## Decision

Public courses use Common Cartridge 1.3 `.imscc` packages. Lessons use standard web content resources. Quizzes use the Common Cartridge QTI 1.2.1 profile.

The application does not add a private manifest or XML extension for terminal runners. Starter files can be packaged as ordinary resources, but runner behavior is not part of the cartridge.

The public JSON catalog is an index, not a course format. It records distribution digests and minimum reader versions outside the cartridge.

## Consequences

- Other Common Cartridge readers can import the learning content.
- Package validation can use published XML schemas and conformance rules.
- Automatic C and SQL checking is not portable and is not available through cartridge metadata.
- New assessment profiles require explicit standards support and graceful failure.
- The earlier terminal editor decision applies only if a future published standard defines interoperable workspace semantics.
