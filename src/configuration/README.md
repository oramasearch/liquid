# Configuration language implementation

`parser.cpp` parses `.liquid` source and resolves repository-local includes. `language.cpp`
performs static checking, lowering, diagnostics, and canonical formatting. `records.cpp` owns the
canonical `target/1` and `source/1` CBOR codecs, including a target's nested canonical
`install_intent/1`. The checker applies the install name, path, mode, restart, and cache grammar
before lowering. Generator execution is kept in `generator.cpp` so
the synchronous parser and checker never wait on the runtime.
