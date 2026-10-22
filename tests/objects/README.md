# Object record tests

`test_records.cpp` checks canonical CBOR records and their round trips. ABI truncation coverage
verifies that complete records omit the new flag, incomplete descriptors retain it, and a
non-boolean flag is rejected.
