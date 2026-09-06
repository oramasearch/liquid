# Configuration-language tests

`test_configuration.cpp` pins twenty complete diagnostics, package/target/source lowering,
strict install-intent diagnostics and lowering, conditional dependency lowering, canonical
`target/1` and `source/1` records, and formatter idempotence. Network refusal, generator
provenance, and relocation-checked configured target cases share the repository and action test
infrastructure in this file.
