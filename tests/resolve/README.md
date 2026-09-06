# Resolution tests

The resolution tests cover spec parsing and positions, canonical records, directory and repository
loading, virtual providers, compiler dependencies, target fallback, conditional variants, explicit
conflicts, cancellation, compatibility facts, lexicographic optimisation, minimal attributed
cores, explanations, exact stable JSON, the CLI, the generated 500-package workload, and a
2,000-package strict dependency chain that keeps graph construction independent of the
worker thread's native stack (issue #263).
