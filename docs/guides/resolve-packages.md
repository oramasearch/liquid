# Resolve packages from declarations

Write two `.liquid` files and resolve the application against their directory. This needs only
an installed `liquid` on `PATH`; it creates no repository and fetches nothing. Run the commands
in a fresh working directory. `app` requires `library` version `2.0`, so the graph contains
`app@1.0` and `library@2.0` with a link dependency between them.

<!-- guides-run: resolve-packages -->
```sh
mkdir universe
cat > universe/library.liquid <<'LIQUID'
package library {
    versions: [1.0, 2.0]
}
LIQUID
cat > universe/app.liquid <<'LIQUID'
package app {
    versions: [1.0]
    depends_on library {
        version: 2.0
        type: link
    }
}
LIQUID
liquid config check universe
liquid resolve app --universe universe --explain
liquid resolve app --universe universe --json > source-graph.json

# Scripts can instead persist each canonical record, including through stdout.
mkdir records
liquid config lower universe/app.liquid --output-record records/app.cbor
liquid config lower universe/library.liquid --output-record - > records/library.cbor
liquid resolve app --universe records --json > record-graph.json
cmp source-graph.json record-graph.json
```

`--output-record <file|->` writes exactly one binary `package/1` record. It writes no summary
to stdout, so redirection preserves the bytes. A file containing several packages requires
`--package <name>` to select one. It cannot be combined with `--json` or `--trace`; those
options retain the human-readable name and origin summary. The destination's parent directory
must already exist.

A universe may mix `.liquid` and `.cbor` files for different package names, including nested
directories. If the selected directory contains `packages/`, that is the scan root. Each package
name must be unique across the universe. A sibling `.cbor` record takes precedence over a
`.liquid` file with the same stem, preserving existing imported universes. Keep generated records
in a separate directory from their declarations so source edits are used immediately. Includes use the configuration language's normal local-path rules;
a declaration reached both through an include and through the scan is loaded once. A declaration
with a generator must first be lowered with `config lower --output-record` in its repository.

An empty directory, or one containing only unsupported files, fails with `No candidates were
loaded from <path>; supply .liquid declarations or canonical .cbor records`. Malformed supported
files report their own decoding or configuration error. If packages load but their requirements
contradict each other, resolution still reports the minimal attributed conflict core.

The [configuration language](../library/configuration-language.md) documents declaration types;
[dependency resolution](../library/resolution.md) describes selection rules and explanations.
