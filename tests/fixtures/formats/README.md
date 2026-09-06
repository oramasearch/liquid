# Format fixtures

These are small files produced once by the reference implementations. Regenerate the ZIP, pax
tar, SQLite, and Parquet files from this directory with:

```sh
python3 generate.py
python3 generate.py --wrappers
UV_CACHE_DIR="$PWD/../../../build-tools-cache" \
  UV_PYTHON_INSTALL_DIR="$PWD/../../../build-tools-python" \
  uv run --python 3.13 --no-project --with 'pyarrow==21.0.0' \
  python generate.py --parquet
```

Generate the two HDF5 variants with the installed HDF5 reference library:

```sh
h5cc hdf5_fixture.c -o hdf5-fixture
./hdf5-fixture sample-a.h5 0
./hdf5-fixture sample-b.h5 1
```

`archive-a.zip` and `archive-b.zip` contain STORED, DEFLATE, and forced-ZIP64 members. They share
the STORED payload and differ only in `deflated.txt`. `stored.zip` is the Tier 1 byte-round-trip
fixture. `descriptor.zip` is written to a non-seekable stream so Python emits a data descriptor.
The two `volatile-*.zip` files have identical content and different timestamps.
`pax.tar` has a path that requires a pax extended header. `pax.tar.gz` and `pax.tar.zst` are
reference wrappers produced by Python gzip and zstd 1.5.x. `sample-a.h5` and
`sample-b.h5` have a group, a shuffle-plus-deflate chunked integer dataset, and a compact
floating-point dataset; one chunked value differs. The Parquet file has two row groups, a
dictionary string column, and uncompressed, gzip, and zstd columns. The SQLite database has two
tables and ordinary rowid table b-trees.
