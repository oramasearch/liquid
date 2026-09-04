# Frontend integration

This directory implements libliquid's checked frontend-module loader and the query-graph
coordinator. Language-specific parsing and compilation do not belong here; they live in separate
targets under [`modules/frontends/`](../../modules/frontends/). The public contract and canonical
values are documented in [`docs/library/frontends.md`](../../docs/library/frontends.md).
