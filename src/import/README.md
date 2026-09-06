# Spack import

This directory contains the synchronous Spack export parser, loss accounting, canonical
configuration rendering, and asynchronous patch retrieval used by `liquid::import_spack`.
Foreign Python never enters libliquid; the importer accepts only the versioned JSON interchange.
