# ADR 0003: Use integrity-bound exercise profiles

## Status

Accepted

## Context

Common Cartridge carries portable lessons and assessments but does not define terminal workspaces or local C and SQL execution. Hands-on Learning needs local exercises without adding executable metadata to the cartridge or accepting package-provided commands.

## Decision

Executable lessons use a versioned JSON exercise profile outside the Common Cartridge. The profile is declarative and can select only application-owned runner IDs, fixed runner profiles, file roles, phases, and check policies. It cannot provide commands, arguments, environment variables, host paths, shell fragments, or compiler flags.

Each profile binds to one cartridge SHA-256, course ID, course version, lesson item IDs, and the exact cartridge resource referenced by each lesson item. Every declared exercise file includes a SHA-256 digest. Catalog entries independently declare the profile URL, byte count, and SHA-256 digest. Cached catalog artifacts are revalidated before reuse.

Catalog-managed profiles can be associated automatically after catalog verification in the startup catalog flow. Courses discovered by the in-reader course picker open content-only; the learner must quit and reopen one from the startup catalog menu to use its verified exercises. A local `--course` remains content-only unless the user also supplies `--exercise-profile PATH`.

C exercises run in fresh Bubblewrap stages. Editable files come from learner workspaces. Readonly and hidden files come from verified cartridge extraction. The sandbox has no network, a private PID namespace and session, private HOME and tmp filesystems, read-only system mounts, a clean environment, fixed process arguments, and resource limits.

For `tests` checks, the runner issues a random per-run completion token and requires the trusted check path to return it only after the supplied checks finish. This prevents an accidental premature successful exit from passing. The completion marker is not an anti-cheat secret. Learner-controlled code executes in the same sandbox as its local test harness, can inspect its environment, and can deliberately forge the marker and local result.

For `stdout` checks, the digest-verified `expected_output` value, loaded from the profile's `expected` field, is the immutable oracle. A separate package test translation unit is not required for the Check phase.

## Consequences

- Common Cartridge remains portable and readable without Hands-on Learning metadata.
- Invalid, mismatched, or undeclared profiles cannot make a lesson executable.
- Trusted test sources are never loaded from learner workspaces.
- Completion tokens detect premature success but do not make local checks tamper-proof against deliberate learner code.
- Bubblewrap and `prlimit` are Linux runtime dependencies for executable lessons.
- Stage input is capped at 64 MiB, process count at 32, individual output files at 16 MiB, and captured output at 1 MiB per stream.
- RLIMIT controls are per process or per file. Without a delegated cgroup, the application cannot enforce a strict aggregate CPU, memory, or writable-filesystem quota across every sandbox descendant. PID isolation, the process limit, timeout supervision, bounded staging, and cleanup reduce this risk but do not replace cgroup accounting.
