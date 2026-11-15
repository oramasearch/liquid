# Test support

`forge_double` is the installable in-memory forge, repository-sync, and operator-control-plane
server used by liquid and downstream liquify integration tests. `set_server_info` sets the body of
its `GET /v1/server_info` (404 when unset) for the named-lake version check, and sync routes record
the bearer that `last_authorization()` returns. Its standalone executable prints
the selected repository URL and remains available until interrupted.
