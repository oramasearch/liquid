# Parquet reader

The private reader decodes compact-protocol metadata and supported column pages for the format
registry. Its footer, flat-schema traversal, and every column page header share one structural
work allowance (liquid #434). Every list, set, map, and struct descent checks the 64-level limit;
schema group depth is checked separately because a flat metadata list can describe a deep tree.
Collection slots are charged before reservation, fields and values before decoding, with at
most 1,000,000 units per unpack. Structural exhaustion returns an error.
