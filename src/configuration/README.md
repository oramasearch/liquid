# Configuration language implementation

`parser.cpp` parses `.liquid` source and resolves repository-local includes. `language.cpp`
performs static checking, lowering, diagnostics, and canonical formatting. `records.cpp` owns the
canonical `target/1` and `source/1` CBOR codecs. Generator execution is kept in `generator.cpp` so
the synchronous parser and checker never wait on the runtime.

Named `lock` declarations lower their repository-relative path patterns into `lock_config`.
Generated configuration merges those rules with source rules in declaration order.
