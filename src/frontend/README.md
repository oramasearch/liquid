# Frontend integration

This directory implements libliquid's checked frontend-module loader and the query-graph
coordinator. Language-specific parsing and compilation do not belong here; they live in separate
targets under [`modules/frontends/`](../../modules/frontends/). The public contract and canonical
values are documented in [`docs/library/frontends.md`](../../docs/library/frontends.md).

`symbols.cpp` validates and encodes the frontend graph's `symbols` query value. Its public record
contract is in [`docs/library/symbols.md`](../../docs/library/symbols.md).
`syntax.cpp` validates the optional ABI v1 concrete-tree record and provides parent-linked
`node_at` and `enclosing` navigation without exposing a parser type.
