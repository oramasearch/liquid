# Deterministic deflate

`deflate.cpp` owns liquid's gzip write path. It emits RFC 1951 stored blocks with fixed block
boundaries and an RFC 1952 header whose timestamp is zero and operating-system byte is 255. The
output therefore does not depend on the host zlib version. zlib remains read-only here and is used
only to validate and decode gzip members.
