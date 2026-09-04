# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, and prints changed manifest
paths for `--explain`. The command never reads the private action cache directly.

The public command table and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).
