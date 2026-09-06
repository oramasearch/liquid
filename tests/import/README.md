# Spack import tests

`fixtures/spack-20.json` is the portable importer fixture. Its golden package documents and
`import-report.json` pin the formatter and loss-accounting contract. `fixtures/spack-500.json`
contains the deterministic 500-package root closure used to prove scale, variant resolution, and
the introduced HDF5 conflict without requiring Spack or network access.

`fixtures/spack-560.json` is the exporter output from Spack revision
`2bfcc69fa870d3c6919be87593f22647981b648a` with its checkout archive pinned by SHA-256. Its eight
documented roots produce 560 exported packages and 540 representable package records. `hip.patch`
supplies the exact bytes for the export's one digest-less patch so the default suite stays offline.

`generate_exports.py` regenerates the twenty-package and exact-500 deterministic documents. The
pinned-checkout fixture is regenerated only with `tools/spack-export/export.py`. Changing the schema
requires updating the parser, exporter, fixtures, and this directory together.
