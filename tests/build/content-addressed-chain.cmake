# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_CONTENT_CHAIN_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_CONTENT_CHAIN_ROOT are required")
endif()

function(run_wrapped prefix project cache)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env LIQUID_WRAP_CACHE=${cache}
      ${LIQUID_CLI} wrap --no-verify ${ARGN}
    WORKING_DIRECTORY ${project}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "wrapped command failed in ${project}:\n${output}\n${error}")
  endif()
  string(REGEX MATCH "processes_started=([0-9]+)" unused_processes "${output}")
  if(unused_processes STREQUAL "")
    message(FATAL_ERROR "wrapped command did not report a process count:\n${output}")
  endif()
  set(${prefix}_processes ${CMAKE_MATCH_1} PARENT_SCOPE)
  string(REGEX MATCH "output_tree=([^ \n]+)" unused_tree "${output}")
  if(unused_tree STREQUAL "" OR CMAKE_MATCH_1 STREQUAL "unavailable")
    message(FATAL_ERROR "wrapped command did not report an output tree:\n${output}")
  endif()
  set(${prefix}_tree ${CMAKE_MATCH_1} PARENT_SCOPE)
  set(${prefix}_output "${output}" PARENT_SCOPE)
endfunction()

function(require_zero label value)
  if(NOT value EQUAL 0)
    message(FATAL_ERROR "${label} started ${value} processes, expected zero")
  endif()
endfunction()

function(require_positive label value)
  if(NOT value GREATER 0)
    message(FATAL_ERROR "${label} started no processes")
  endif()
endfunction()

file(REMOVE_RECURSE "${LIQUID_CONTENT_CHAIN_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_CONTENT_CHAIN_ROOT}/downloads")
file(MAKE_DIRECTORY "${LIQUID_CONTENT_CHAIN_ROOT}/sources")
file(MAKE_DIRECTORY "${LIQUID_CONTENT_CHAIN_ROOT}/projects")
file(MAKE_DIRECTORY "${LIQUID_CONTENT_CHAIN_ROOT}/caches")

set(zlib_archive "${LIQUID_CONTENT_CHAIN_ROOT}/downloads/zlib-1.3.1.tar.gz")
set(png_archive "${LIQUID_CONTENT_CHAIN_ROOT}/downloads/libpng-1.6.44.tar.gz")
file(DOWNLOAD
  https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1
  "${zlib_archive}"
  EXPECTED_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c
  STATUS zlib_download)
list(GET zlib_download 0 zlib_status)
if(NOT zlib_status EQUAL 0)
  message(FATAL_ERROR "zlib download failed: ${zlib_download}")
endif()
file(DOWNLOAD
  https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.44
  "${png_archive}"
  EXPECTED_HASH SHA256=0ef5b633d0c65f780c4fced27ff832998e71478c13b45dfb6e94f23a82f64f7c
  STATUS png_download)
list(GET png_download 0 png_status)
if(NOT png_status EQUAL 0)
  message(FATAL_ERROR "libpng download failed: ${png_download}")
endif()
file(ARCHIVE_EXTRACT INPUT "${zlib_archive}" DESTINATION "${LIQUID_CONTENT_CHAIN_ROOT}/sources")
file(ARCHIVE_EXTRACT INPUT "${png_archive}" DESTINATION "${LIQUID_CONTENT_CHAIN_ROOT}/sources")

set(zlib_source "${LIQUID_CONTENT_CHAIN_ROOT}/sources/zlib-1.3.1")
set(png_source "${LIQUID_CONTENT_CHAIN_ROOT}/sources/libpng-1.6.44")
set(zlib "${LIQUID_CONTENT_CHAIN_ROOT}/projects/zlib")
set(png "${LIQUID_CONTENT_CHAIN_ROOT}/projects/libpng")
set(program "${LIQUID_CONTENT_CHAIN_ROOT}/projects/program")
file(MAKE_DIRECTORY "${zlib}")
file(MAKE_DIRECTORY "${png}")
file(MAKE_DIRECTORY "${program}")
foreach(source adler32.c zutil.h zlib.h zconf.h)
  file(COPY_FILE "${zlib_source}/${source}" "${zlib}/${source}")
endforeach()
foreach(source pngget.c pngpriv.h png.h pngconf.h pngstruct.h pnginfo.h pngdebug.h)
  file(COPY_FILE "${png_source}/${source}" "${png}/${source}")
endforeach()
file(COPY_FILE "${png_source}/scripts/pnglibconf.h.prebuilt" "${png}/pnglibconf.h")

file(WRITE "${zlib}/Liquid.mk" [=[
CC ?= clang
AR ?= ar
CFLAGS := -O2 -fPIC -I. -DHAVE_UNISTD_H -DSTDC
sources := adler32.c
objects := $(sources:%.c=.liquid-build/%.o)

.PHONY: all
all: .liquid-build/libz.a
	@mkdir -p export/lib export/include
	cp .liquid-build/libz.a export/lib/libz.a
	cp zlib.h zconf.h export/include/

.liquid-build/%.o: %.c
	@mkdir -p .liquid-build
	$(CC) $(CFLAGS) -c $< -o $@

.liquid-build/libz.a: $(objects)
	$(AR) rcsD $@ $^
]=])

file(WRITE "${png}/Liquid.mk" [=[
CC ?= clang
AR ?= ar
CFLAGS := -O2 -fPIC -I. -Ideps/zlib/export/include
sources := pngget.c
objects := $(sources:%.c=.liquid-build/%.o)

.PHONY: all
all: .liquid-build/libpng.a
	@mkdir -p export/lib export/include
	cp .liquid-build/libpng.a export/lib/libpng.a
	cp png.h pngconf.h pnglibconf.h export/include/
	cp deps/zlib/export/lib/libz.a export/lib/libz.a
	cp deps/zlib/.liquid-build/adler32.o export/lib/adler32.o
	cp deps/zlib/export/include/zlib.h deps/zlib/export/include/zconf.h export/include/

.liquid-build/%.o: %.c
	@mkdir -p .liquid-build
	$(CC) $(CFLAGS) -c $< -o $@

.liquid-build/libpng.a: $(objects)
	$(AR) rcsD $@ $^
]=])

file(WRITE "${program}/main.c" [=[
#include <zlib.h>

int main(void) {
    return adler32(0L, Z_NULL, 0) == 1L ? 0 : 1;
}
]=])
file(WRITE "${program}/Liquid.mk" [=[
CC ?= clang
CFLAGS := -O2 -Ideps/png/export/include

.PHONY: all
all: app

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

app: main.o
	$(CC) $< deps/png/export/lib/adler32.o -o $@
]=])

set(zlib_cache "${LIQUID_CONTENT_CHAIN_ROOT}/caches")
set(png_cache "${LIQUID_CONTENT_CHAIN_ROOT}/caches")
set(program_cache "${LIQUID_CONTENT_CHAIN_ROOT}/caches")

run_wrapped(zlib_build_initial "${zlib}" "${zlib_cache}" make -f Liquid.mk -j2)
require_positive("initial zlib build" ${zlib_build_initial_processes})
run_wrapped(png_build_initial "${png}" "${png_cache}"
  --input zlib=${zlib_build_initial_tree} make -f Liquid.mk -j2)
require_positive("initial libpng build" ${png_build_initial_processes})
run_wrapped(program_initial "${program}" "${program_cache}"
  --input png=${png_build_initial_tree} make -f Liquid.mk)
require_positive("initial program build" ${program_initial_processes})

file(APPEND "${zlib}/adler32.c" "\n/* content-addressed output measurement */\n")
run_wrapped(zlib_build_comment "${zlib}" "${zlib_cache}" make -f Liquid.mk -j2)
require_positive("comment-only zlib build" ${zlib_build_comment_processes})
if(NOT zlib_build_comment_tree STREQUAL zlib_build_initial_tree)
  message(FATAL_ERROR
    "comment-only zlib rebuild changed the normalised output tree:\n${zlib_build_initial_tree}\n${zlib_build_comment_tree}")
endif()

run_wrapped(png_build_comment "${png}" "${png_cache}"
  --input zlib=${zlib_build_comment_tree} make -f Liquid.mk -j2)
require_zero("libpng build after comment-only upstream change" ${png_build_comment_processes})
file(REMOVE "${program}/app" "${program}/main.o")
run_wrapped(program_comment "${program}" "${program_cache}"
  --input png=${png_build_comment_tree} make -f Liquid.mk)
require_zero("program build after comment-only upstream change" ${program_comment_processes})

file(READ "${zlib}/adler32.c" adler_source)
set(adler_signature
  "uLong ZEXPORT adler32_z(uLong adler, const Bytef *buf, z_size_t len) {\n")
set(adler_changed
  "uLong ZEXPORT adler32_z(uLong adler, const Bytef *buf, z_size_t len) {\n    volatile unsigned liquid_semantic_marker = 1;\n    (void)liquid_semantic_marker;\n")
string(REPLACE "${adler_signature}" "${adler_changed}" changed_adler "${adler_source}")
if(changed_adler STREQUAL adler_source)
  message(FATAL_ERROR "zlib semantic fixture could not locate adler32_z")
endif()
file(WRITE "${zlib}/adler32.c" "${changed_adler}")
run_wrapped(zlib_build_semantic "${zlib}" "${zlib_cache}" make -f Liquid.mk -j2)
require_positive("semantic zlib build" ${zlib_build_semantic_processes})
if(zlib_build_semantic_tree STREQUAL zlib_build_comment_tree)
  message(FATAL_ERROR "semantic zlib rebuild did not change the output tree")
endif()

run_wrapped(png_build_semantic "${png}" "${png_cache}"
  --input zlib=${zlib_build_semantic_tree} make -f Liquid.mk -j2)
file(REMOVE "${program}/app" "${program}/main.o")
run_wrapped(program_semantic "${program}" "${program_cache}"
  --input png=${png_build_semantic_tree} make -f Liquid.mk)
require_positive("program link after semantic upstream change" ${program_semantic_processes})

if(EXISTS "/proc/loadavg")
  file(READ "/proc/loadavg" load_average)
  string(STRIP "${load_average}" load_average)
else()
  execute_process(COMMAND uptime OUTPUT_VARIABLE load_average OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

message(STATUS
  "CHAIN initial zlib=${zlib_build_initial_processes} libpng=${png_build_initial_processes} program=${program_initial_processes}")
message(STATUS
  "CHAIN comment zlib=${zlib_build_comment_processes} libpng=${png_build_comment_processes} program=${program_comment_processes}")
message(STATUS
  "CHAIN semantic zlib=${zlib_build_semantic_processes} libpng=${png_build_semantic_processes} program=${program_semantic_processes}")
message(STATUS "CHAIN load_average=${load_average}")
