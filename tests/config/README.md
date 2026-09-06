# Configuration-language tests

`test_configuration.cpp` pins twenty complete diagnostics, package/target/source lowering,
conditional dependency lowering, canonical `target/1` and `source/1` records, and formatter
idempotence. Network refusal, generator provenance, and relocation-checked configured target cases
share the repository and action test infrastructure in this file. Source coverage also proves that
content already present under its SHA-256 is reused without touching its deliberately invalid URL.
