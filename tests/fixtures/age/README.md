# age interoperability fixtures

`x25519.b64` and `x25519-multiple-recipients.b64` are Base64 encodings of the
files with the corresponding names under `age/testdata/` in the C2SP CCTV
repository at commit `4448f2097b2daa812c91a26141f9f36c2096b9ca`.

Source: <https://github.com/C2SP/CCTV/tree/4448f2097b2daa812c91a26141f9f36c2096b9ca/age/testdata>

The encoding keeps the binary payload byte-for-byte while allowing the fixture
to remain reviewable as text. The tests decode it before passing the age file to
the custody implementation.
