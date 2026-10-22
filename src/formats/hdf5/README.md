# HDF5 reader

The private reader decodes superblocks, version 1 and 2 object headers, groups, and contiguous,
compact and chunked datasets for the format registry. Group nesting, group B-trees and chunk
B-trees each reject a 64th level in addition to their cycle checks, because distinct objects or
nodes can chain without a cycle (liquid #434). A chunk B-tree past the limit makes its dataset
opaque; a group past the limit fails the unpack. Object counts are bounded by the unpack's entry
limit.
