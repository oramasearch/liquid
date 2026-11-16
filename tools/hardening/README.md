# Hardening lane

Linux sanitizer and fuzzing runs on a BuildKit daemon, without Docker on the development machine
(liquid [#407](https://github.com/oramasearch/liquid/issues/407)). No CI runs them: they are run by
hand, locally or here.

| File | What it does |
|---|---|
| [`Dockerfile`](Dockerfile) | Ubuntu 24.04 with Clang 19. `MODE=ctest` builds with `CMAKE_OPTIONS` and runs the whole suite, then prints the ELF protections of the shipped programs; `MODE=fuzz` builds the harnesses and runs each for `FUZZ_SECONDS`. |
| [`run.sh`](run.sh) | Copies a checkout and runs the Dockerfile through `buildctl`, writing a log and printing a summary. |

Commands and recorded results: [`docs/build/hardening.md`](../../docs/build/hardening.md).
