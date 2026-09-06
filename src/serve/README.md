# Server control transport

`http.cpp` contains the bounded one-request HTTP/1.1 parser, response writer, asynchronous route
table, and Corosio TCP and Unix-socket accept loops shared by liquid's long-running components.
Component-specific health, metrics, REST, OpenAPI, and MCP handlers remain in their owning source
folders and register only the routes they expose.

Handlers are Capy tasks, so a route can await store and repository operations without blocking the
runtime executor. Filesystem preparation for a Unix socket uses `runtime::offload`.
