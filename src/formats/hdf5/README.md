# HDF5 reader

`hdf5.cpp` implements the dependency-free Tier 2 reader described in
[`docs/library/formats.md`](../../../docs/library/formats.md#hdf5).

Both v1 and v2 filter pipeline messages are limited to 32 filters before any dataset data is
read. Every chunk filter index therefore fits its 32-bit skip mask. Larger pipelines return
`errc::corrupt`, including on compact and contiguous datasets (liquid #486).
