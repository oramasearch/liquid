# Resolution tests

The resolution tests cover spec parsing and positions, canonical records, directory and repository
loading, virtual providers, compiler dependencies, target fallback, conditional variants, explicit
conflicts, cancellation, compatibility facts, lexicographic optimisation, minimal attributed
cores, explanations, exact stable JSON, the CLI, the generated 500-package workload, and a
2,000-package strict dependency chain that keeps graph construction independent of the
worker thread's native stack (issue #263).

Issue #287 covers mixed declaration/record universes, included declarations discovered once,
checker diagnostics, duplicate names across formats and source files, and repository declaration
resolution against canonical dependencies.

Sibling canonical records take precedence over declarations with the same stem, retaining
compatibility with the paired Spack import layout. Other duplicate package names remain errors.
