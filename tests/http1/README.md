# HTTP/1.1 tests

`test_client.cpp` covers the HTTP/1.1 client's response parser, `http1::parse_response`: a
Content-Length body, a chunked body, and malformed status lines and fields from a server, which
are reported as corrupt responses instead of escaping as exceptions (liquid
[#407](https://github.com/oramasearch/liquid/issues/407), found by `fuzz/http1.cpp`).
