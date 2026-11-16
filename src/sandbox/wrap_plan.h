// SPDX-License-Identifier: MIT

#pragma once

/* The liquid wrap plan: the rows the exec-time hook reads to decide whether a process is a cache
 * hit, and the checks that decide whether a plan file and its conditional inputs can be trusted
 * (liquid#401), and the process key that names each wrapped process (liquid#449). Plain C so the
 * preloaded hook and libliquid share one implementation; see docs/library/build-engine.md, "The
 * wrap plan".
 *
 * Plan rows, one per line:
 *   H\t<process key, 64 hex>\t<exit status>
 *   C\t<process key, 64 hex>\t<BLAKE3-256 of the input, 64 hex>\t<size>\t<relative path, hex>
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIQUID_WRAP_DIGEST_HEX_SIZE 64

/* A process key is a BLAKE3-256 digest, written as 64 lowercase hex digits (liquid#449). */
#define LIQUID_WRAP_KEY_HEX_SIZE LIQUID_WRAP_DIGEST_HEX_SIZE

/* The environment variable that carries the BLAKE3-256 digest of the plan file's bytes. */
#define LIQUID_WRAP_PLAN_DIGEST_VARIABLE "LIQUID_WRAP_PLAN_DIGEST"

/* Writes the lowercase hex BLAKE3-256 digest of `size` bytes and a terminating NUL to `output`. */
void liquid_wrap_digest_hex(const void* data,
                            size_t size,
                            char output[LIQUID_WRAP_DIGEST_HEX_SIZE + 1]);

/* Hashes the file at `path` with BLAKE3-256. Returns 0 and fills `output` and `size`, or -1. */
int liquid_wrap_file_digest_hex(const char* path,
                                char output[LIQUID_WRAP_DIGEST_HEX_SIZE + 1],
                                uint64_t* size);

/* Reads the plan at `path` and returns its bytes (NUL-terminated, caller frees) only when they
 * hash to `expected_hex`. The file is opened without following a final symbolic link and must
 * be a regular file; with `require_private` it must also be owned by the effective uid and carry
 * no group or other permission. Returns NULL when any check fails or `expected_hex` is NULL. */
char* liquid_wrap_plan_load(const char* path,
                            const char* expected_hex,
                            int require_private,
                            size_t* size);

/* Writes the process key of a command, the lowercase hex BLAKE3-256 digest of its executable,
 * working directory and NUL-separated argument bytes, and a terminating NUL to `output`. Each part
 * is hashed with its length first, under a version label, so no two commands share the input. */
void liquid_wrap_process_key_hex(const char* executable,
                                 size_t executable_size,
                                 const char* directory,
                                 size_t directory_size,
                                 const char* arguments,
                                 size_t arguments_size,
                                 char output[LIQUID_WRAP_KEY_HEX_SIZE + 1]);

/* Returns the recorded exit status when `plan` has an H row for `key` (64 lowercase hex digits)
 * and every C row for `key` names a file under `root` whose size and BLAKE3-256 digest match;
 * otherwise -1. A row whose key is not 64 hex digits never matches, so a plan in the old 64-bit
 * form has no hits. */
int liquid_wrap_plan_status(const char* plan, size_t size, const char* key, const char* root);

#ifdef __cplusplus
}
#endif
