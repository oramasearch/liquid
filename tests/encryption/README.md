# Encryption tests

`test_leaf.cpp` and `test_repository_leaf.cpp` cover structured and whole-file encryption.
`test_custody.cpp` covers local provider round trips, age CCTV fixtures, Shamir quorum behavior,
all wrapped-key versions, and every ordered cross-kind rewrap while comparing pack bytes.
`test_remote_custody.cpp` runs only on Unix and supplies loopback-only AWS, GCP, Azure, and lake
HTTP doubles that validate authentication and request shape before echoing test ciphertext. It
also checks that rejected remote credentials return `unauthorized` for the exact key version.
