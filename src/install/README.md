# Transactional installation

`records.cpp` validates and canonically encodes the closed `install_intent/1` vocabulary.
`install.cpp` reads projected package payloads or install trees on the runtime blocking boundary,
stages complete generations, applies intent within the staged root, and atomically selects or
rolls back `current`. `faults.cpp` provides the deterministic file and switch injection points used
by the `SIGKILL` recovery proof.

No caller shell is represented or executed. The public contract and recovery invariants are in
`docs/library/projection.md`.
