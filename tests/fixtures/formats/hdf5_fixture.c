#include <hdf5.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failed(herr_t value, const char* operation) {
    if (value >= 0) {
        return 0;
    }
    fprintf(stderr, "%s failed\n", operation);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: hdf5-fixture <output> <variant>\n");
        return 2;
    }
    const int variant = atoi(argv[2]);
    hid_t access = H5Pcreate(H5P_FILE_ACCESS);
    if (access < 0 || failed(H5Pset_libver_bounds(access, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
                             "set version bounds")) {
        return 1;
    }
    hid_t file = H5Fcreate(argv[1], H5F_ACC_TRUNC, H5P_DEFAULT, access);
    if (file < 0) {
        return 1;
    }
    hid_t group = H5Gcreate2(file, "/measurements", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dimensions[2] = {4, 4};
    hid_t space = H5Screate_simple(2, dimensions, NULL);
    hid_t creation = H5Pcreate(H5P_DATASET_CREATE);
    hsize_t chunks[2] = {2, 2};
    failed(H5Pset_chunk(creation, 2, chunks), "set chunks");
    failed(H5Pset_shuffle(creation), "set shuffle");
    failed(H5Pset_deflate(creation, 6), "set deflate");
    hid_t chunked =
        H5Dcreate2(group, "chunked", H5T_STD_I32LE, space, H5P_DEFAULT, creation, H5P_DEFAULT);
    int32_t values[16];
    for (int index = 0; index < 16; ++index) {
        values[index] = index;
    }
    if (variant != 0) {
        values[10] = 1000;
    }
    failed(H5Dwrite(chunked, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, values),
           "write chunked dataset");

    hsize_t compact_dimensions[1] = {4};
    hid_t compact_space = H5Screate_simple(1, compact_dimensions, NULL);
    hid_t compact_creation = H5Pcreate(H5P_DATASET_CREATE);
    failed(H5Pset_layout(compact_creation, H5D_COMPACT), "set compact layout");
    hid_t compact = H5Dcreate2(group,
                               "compact",
                               H5T_IEEE_F64LE,
                               compact_space,
                               H5P_DEFAULT,
                               compact_creation,
                               H5P_DEFAULT);
    double compact_values[4] = {1.25, 2.5, 5.0, 10.0};
    failed(H5Dwrite(compact, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, compact_values),
           "write compact dataset");

    H5Dclose(compact);
    H5Pclose(compact_creation);
    H5Sclose(compact_space);
    H5Dclose(chunked);
    H5Pclose(creation);
    H5Sclose(space);
    H5Gclose(group);
    H5Fclose(file);
    H5Pclose(access);
    return 0;
}
