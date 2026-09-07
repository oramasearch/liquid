# Platform installer tests

The fixture driver projects a native test program into a macOS component package or Windows MSIX.
The CI scripts create temporary self-signed identities, sign outside projection, trust the test
certificate only for the duration of the job, install the package, run the installed program, and
verify the `platform_signature/1` sidecar with the native verifier. The macOS script also compares
two unsigned packages byte for byte. It deliberately disables notarisation because pull-request CI
has no release credentials.
