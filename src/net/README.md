# Private network client

`http_client.hpp` provides URL decomposition plus bearer-authenticated JSON requests and lake
envelope errors for the forge client. It delegates URL connection, TLS peer verification, response
framing, and chunked decoding to the private `src/http1/client.cpp` transport. Public headers expose
only Capy tasks and liquid values.
