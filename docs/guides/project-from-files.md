# Package ordinary repository files

Start in an empty directory with `liquid` on `PATH`. These commands record ordinary files,
name a change, and produce a Debian package and an OCI image layout. No build configuration
or filesystem mount is needed. Put package outputs outside the recorded directory.

<!-- guides-run: project-from-files -->
```sh
mkdir package
cd package
mkdir -p share/example
printf 'payload\n' > share/example/message.txt
liquid init .
change=$(liquid cut release)
liquid project deb "${change#change=}" --name example --version 1.0.0 --arch arm64 --out ../out
liquid project oci "${change#change=}" --name example --version 1.0.0 --arch arm64 --out ../image
tree=$(liquid tree)
liquid project deb "$tree" --name example --version 1.0.0 --arch arm64 --out ../tree-out
cmp ../out/example-1.0.0-1.arm64.deb ../tree-out/example-1.0.0-1.arm64.deb
```

`liquid tree` prints the canonical digest of the recorded root, including its algorithm prefix.
It reads repository state; it does not import unrecorded filesystem edits. `cut` prints
`change=<digest>`, so the shell expansion removes that label before projection. The Debian
packages produced from the change and its root tree have identical bytes.

See the [project reference](../reference/cli/project.md) for supported targets and metadata,
and the [tree reference](../reference/cli/tree.md) for the digest command. Debian source
packaging accepts a recorded change, a result, or an install tree with a producing result;
a bare tree without that evidence is not a source-package input.

The [verification record](../build/project-ordinary-repository.md) contains the guide output and
the before/after command transcripts.
