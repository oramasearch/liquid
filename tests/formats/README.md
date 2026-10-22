# Format codec tests

`test_registry.cpp` exercises built-in and loaded codecs through the public format registry;
`test_json.cpp` and `test_dotenv.cpp` cover structured text decoding.

`FormatRegistry.BoundsHdf5FilterPipelinesToTheChunkMask` builds HDF5 v1/v2 filter pipeline
messages with 0, 1, 31, 32, 33, 64 and 255 filters, in compact and chunked datasets. Valid pipelines
preserve the decoded bytes with zero, high-bit and all-bit masks; counts above 32 must return
`errc::corrupt` before chunk decoding. Run with UBSan on `src/formats/hdf5/hdf5.cpp` to check the
mask shifts (liquid #486).
