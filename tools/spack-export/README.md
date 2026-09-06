# Spack exporter

`export.py` is the Python escape hatch that reads package metadata through Spack's own API. Run it
with `spack python` from a pinned Spack checkout; it is not imported by libliquid and Python is not
needed when a `spack-export/1` document is consumed.

The checkout must be supplied to the wrapped action as a digest-pinned input. Pass that digest to
the exporter so the JSON carries both it and Spack's Git revision:

```sh
spack python tools/spack-export/export.py \
  --spack-checkout-digest sha256:<checkout-digest> \
  --root 'hdf5+mpi' --root netcdf-c --root openblas --root fftw \
  --root python --root cmake --root boost --root openssl \
  --output spack-export.json
```

The wrapped action result supplies `slsa_provenance/1`. The JSON's provenance object identifies
that carrier; it is not a second attestation. The exporter records literal CMake or configure
arguments and reports computed Python values, custom phases, and unknown build systems as losses.
Never execute those values to invent a static translation.

The exporter emits one JSON document so it can be reviewed, archived, and imported without a
Spack installation. Closure discovery walks every dependency declaration and virtual provider,
rather than selecting one concrete Spack install, so variant-conditioned choices remain available
to liquid's resolver. Its schema and the import mapping are documented in
[`docs/library/import-spack.md`](../../docs/library/import-spack.md).
