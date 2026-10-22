# SQLite reader

The private reader decodes UTF-8 table b-trees for the format registry. It does not execute SQL or
read index b-trees. Traversal rejects page cycles and, separately, a 64th b-tree level, because
distinct interior pages can chain without a cycle up to the page count (liquid #434). Cell counts
and rows are bounded by the unpack's entry limit.
