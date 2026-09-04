# Protobuf wire codec

This directory contains liquid's dependency-free protobuf wire reader and writer. The codec
supports varints, zigzag integers, fixed-width values, length-delimited fields, packed repeated
values, deprecated groups when skipping unknown fields, and byte-for-byte unknown-field retention.
It has no schema knowledge; the plain REAPI messages in `src/reapi/` provide that layer.
