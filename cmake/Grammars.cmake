# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

set(LIQUID_GRAMMARS "all" CACHE STRING
    "Semicolon-separated tree-sitter grammar subset, or all")
set(LIQUID_GRAMMAR_MIRROR "" CACHE STRING
    "Archive root replacing https://github.com for grammar downloads")

set(LIQUID_GRAMMAR_LOCK
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/frontends/grammars/grammars.lock")
file(SHA256 "${LIQUID_GRAMMAR_LOCK}" LIQUID_GRAMMAR_LOCK_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LIQUID_GRAMMAR_LOCK}")

function(liquid_grammar_archive_url repository commit output)
  if(LIQUID_GRAMMAR_MIRROR)
    string(REGEX REPLACE "/+$" "" mirror "${LIQUID_GRAMMAR_MIRROR}")
    string(REPLACE "https://github.com/" "" repository_path "${repository}")
    set(${output} "${mirror}/${repository_path}/archive/${commit}.tar.gz" PARENT_SCOPE)
  else()
    set(${output} "${repository}/archive/${commit}.tar.gz" PARENT_SCOPE)
  endif()
endfunction()

# Applies one locked patch to a fetched grammar file (liquid#462): an exact replacement of `find`,
# which must occur once, in a file whose SHA-256 is the upstream digest the lock records. A file that
# already has the patched digest was patched on an earlier configure. Any other content is refused,
# so a patch never lands on bytes nobody reviewed.
function(liquid_apply_grammar_patch language file find replace upstream_sha256 patched_sha256)
  file(SHA256 "${file}" actual)
  if(actual STREQUAL patched_sha256)
    return()
  endif()
  if(NOT actual STREQUAL upstream_sha256)
    message(FATAL_ERROR
      "grammar patch for ${language}: ${file} has sha256:${actual}, not the locked upstream "
      "sha256:${upstream_sha256}")
  endif()
  file(READ "${file}" contents)
  string(FIND "${contents}" "${find}" first)
  string(FIND "${contents}" "${find}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR
      "grammar patch for ${language}: the text to replace must occur exactly once in ${file}")
  endif()
  string(REPLACE "${find}" "${replace}" contents "${contents}")
  file(WRITE "${file}" "${contents}")
  file(SHA256 "${file}" actual)
  if(NOT actual STREQUAL patched_sha256)
    message(FATAL_ERROR
      "grammar patch for ${language}: patched ${file} has sha256:${actual}, not the locked "
      "sha256:${patched_sha256}")
  endif()
endfunction()

function(liquid_configure_grammars)
  cmake_parse_arguments(ARG "" "OUT_RUNTIME_SOURCE;OUT_RUNTIME_INCLUDE;OUT_SOURCES;OUT_INCLUDES;OUT_GENERATED;OUT_SOURCE_MAP" "" ${ARGN})
  file(READ "${LIQUID_GRAMMAR_LOCK}" lock_json)
  string(JSON lock_version GET "${lock_json}" version)
  if(NOT lock_version EQUAL 1)
    message(FATAL_ERROR "unsupported grammar lock version: ${lock_version}")
  endif()

  string(JSON runtime_repository GET "${lock_json}" runtime repository)
  string(JSON runtime_commit GET "${lock_json}" runtime commit)
  string(JSON runtime_archive_sha256 GET "${lock_json}" runtime archive_sha256)
  liquid_grammar_archive_url("${runtime_repository}" "${runtime_commit}" runtime_url)
  FetchContent_Declare(liquid_tree_sitter_runtime
    URL "${runtime_url}"
    URL_HASH "SHA256=${runtime_archive_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR liquid-no-upstream-cmake)
  FetchContent_MakeAvailable(liquid_tree_sitter_runtime)

  string(JSON grammar_count LENGTH "${lock_json}" grammars)
  math(EXPR grammar_last "${grammar_count} - 1")
  set(available_languages)
  foreach(index RANGE ${grammar_last})
    string(JSON language GET "${lock_json}" grammars ${index} language)
    list(APPEND available_languages "${language}")
  endforeach()

  if(LIQUID_GRAMMARS STREQUAL "all")
    set(selected_languages ${available_languages})
  else()
    set(selected_languages ${LIQUID_GRAMMARS})
    foreach(language IN LISTS selected_languages)
      if(NOT language IN_LIST available_languages)
        message(FATAL_ERROR
          "LIQUID_GRAMMARS contains unknown language '${language}'; available: ${available_languages}")
      endif()
    endforeach()
  endif()

  set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/frontends")
  file(MAKE_DIRECTORY "${generated_directory}")
  set(generated_source "${generated_directory}/grammar_registry.cpp")
  set(source_map "${generated_directory}/grammar-sources.txt")
  file(WRITE "${generated_source}"
    "#include \"common/grammar_registry.hpp\"\n\n#include <array>\n#include <cstdint>\n#include <string_view>\n\n")
  file(WRITE "${source_map}" "")

  set(grammar_sources)
  set(selected_indexes)
  foreach(index RANGE ${grammar_last})
    string(JSON language GET "${lock_json}" grammars ${index} language)
    if(NOT language IN_LIST selected_languages)
      continue()
    endif()
    list(APPEND selected_indexes ${index})
    string(JSON repository GET "${lock_json}" grammars ${index} repository)
    string(JSON commit GET "${lock_json}" grammars ${index} commit)
    string(JSON archive_sha256 GET "${lock_json}" grammars ${index} archive_sha256)
    string(MAKE_C_IDENTIFIER "${language}" identifier)
    set(content_name "liquid_grammar_${index}_${identifier}")
    liquid_grammar_archive_url("${repository}" "${commit}" grammar_url)
    FetchContent_Declare(${content_name}
      URL "${grammar_url}"
      URL_HASH "SHA256=${archive_sha256}"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR liquid-no-upstream-cmake)
    FetchContent_MakeAvailable(${content_name})
    FetchContent_GetProperties(${content_name} SOURCE_DIR grammar_source_dir)
    set(grammar_source_dir_${index} "${grammar_source_dir}")
    string(JSON patch_type ERROR_VARIABLE patch_error TYPE "${lock_json}" grammars ${index} patches)
    if(NOT patch_error AND patch_type STREQUAL "ARRAY")
      string(JSON patch_count LENGTH "${lock_json}" grammars ${index} patches)
      if(patch_count GREATER 0)
        math(EXPR patch_last "${patch_count} - 1")
        foreach(patch_index RANGE ${patch_last})
          foreach(field path find replace upstream_sha256 sha256)
            string(JSON patch_${field} GET "${lock_json}" grammars ${index} patches ${patch_index} ${field})
          endforeach()
          liquid_apply_grammar_patch("${language}" "${grammar_source_dir}/${patch_path}"
            "${patch_find}" "${patch_replace}" "${patch_upstream_sha256}" "${patch_sha256}")
        endforeach()
      endif()
    endif()
    file(APPEND "${source_map}" "${language}|${grammar_source_dir}\n")

    string(JSON parser_path GET "${lock_json}" grammars ${index} parser path)
    set(parser_source "${grammar_source_dir}/${parser_path}")
    list(APPEND grammar_sources "${parser_source}")
    get_filename_component(parser_directory "${grammar_source_dir}/${parser_path}" DIRECTORY)
    file(SHA256 "${parser_directory}/node-types.json" node_types_sha256)
    set(grammar_node_types_sha256_${index} "${node_types_sha256}")
    set(grammar_source_includes "${grammar_source_dir};${parser_directory}")
    set_property(SOURCE "${parser_source}" PROPERTY INCLUDE_DIRECTORIES "${grammar_source_includes}")
    string(JSON scanner_count LENGTH "${lock_json}" grammars ${index} scanners)
    if(scanner_count GREATER 0)
      math(EXPR scanner_last "${scanner_count} - 1")
      foreach(scanner_index RANGE ${scanner_last})
        string(JSON scanner_path GET "${lock_json}" grammars ${index} scanners ${scanner_index} path)
        set(scanner_source "${grammar_source_dir}/${scanner_path}")
        list(APPEND grammar_sources "${scanner_source}")
        get_filename_component(scanner_directory "${grammar_source_dir}/${scanner_path}" DIRECTORY)
        set_property(SOURCE "${scanner_source}" PROPERTY INCLUDE_DIRECTORIES
          "${grammar_source_includes};${scanner_directory}")
      endforeach()
    endif()
  endforeach()

  foreach(index IN LISTS selected_indexes)
    string(JSON symbol GET "${lock_json}" grammars ${index} symbol)
    file(APPEND "${generated_source}" "extern \"C\" const TSLanguage* ${symbol}(void);\n")
  endforeach()
  file(APPEND "${generated_source}" "\nnamespace liquid_frontend {\nnamespace {\n")

  set(generated_index 0)
  foreach(index IN LISTS selected_indexes)
    string(JSON language GET "${lock_json}" grammars ${index} language)
    string(JSON extension_count LENGTH "${lock_json}" grammars ${index} extensions)
    file(APPEND "${generated_source}"
      "constexpr std::array<std::string_view, ${extension_count}> extensions_${generated_index} = {")
    if(extension_count GREATER 0)
      math(EXPR extension_last "${extension_count} - 1")
      foreach(extension_index RANGE ${extension_last})
        string(JSON extension GET "${lock_json}" grammars ${index} extensions ${extension_index})
        string(REPLACE "\\" "\\\\" extension "${extension}")
        string(REPLACE "\"" "\\\"" extension "${extension}")
        file(APPEND "${generated_source}" "\"${extension}\",")
      endforeach()
    endif()
    file(APPEND "${generated_source}" "};\n")

    string(JSON tags_type TYPE "${lock_json}" grammars ${index} tags)
    if(tags_type STREQUAL "NULL")
      file(APPEND "${generated_source}"
        "constexpr std::array<std::uint8_t, 0> tags_${generated_index} = {};\n")
    else()
      string(JSON tags_path GET "${lock_json}" grammars ${index} tags path)
      set(entry_source_dir "${grammar_source_dir_${index}}")
      file(READ "${entry_source_dir}/${tags_path}" tags_hex HEX)
      string(LENGTH "${tags_hex}" tags_hex_length)
      math(EXPR tags_size "${tags_hex_length} / 2")
      string(REGEX REPLACE "(..)" "0x\\1," tags_values "${tags_hex}")
      file(APPEND "${generated_source}"
        "constexpr std::array<std::uint8_t, ${tags_size}> tags_${generated_index} = {${tags_values}};\n")
    endif()
    math(EXPR generated_index "${generated_index} + 1")
  endforeach()

  file(APPEND "${generated_source}" "\nconst std::array<grammar_descriptor, ${generated_index}> grammar_values = {{\n")
  set(generated_index 0)
  foreach(index IN LISTS selected_indexes)
    string(JSON language GET "${lock_json}" grammars ${index} language)
    string(JSON symbol GET "${lock_json}" grammars ${index} symbol)
    string(JSON abi GET "${lock_json}" grammars ${index} tree_sitter_abi)
    set(node_types_sha256 "${grammar_node_types_sha256_${index}}")
    file(APPEND "${generated_source}"
      "    {\"${language}\", extensions_${generated_index}, &${symbol}, tags_${generated_index}, \"sha256:${node_types_sha256}\", ${abi}},\n")
    math(EXPR generated_index "${generated_index} + 1")
  endforeach()
  file(APPEND "${generated_source}"
    "}};\n}\n\nauto available_grammars() -> std::span<const grammar_descriptor> {\n    return grammar_values;\n}\n\nauto grammar_lock_digest() -> std::string_view {\n    return \"sha256:${LIQUID_GRAMMAR_LOCK_SHA256}\";\n}\n\n} // namespace liquid_frontend\n")

  set(${ARG_OUT_RUNTIME_SOURCE} "${liquid_tree_sitter_runtime_SOURCE_DIR}/lib/src/lib.c" PARENT_SCOPE)
  set(${ARG_OUT_RUNTIME_INCLUDE} "${liquid_tree_sitter_runtime_SOURCE_DIR}/lib/include;${liquid_tree_sitter_runtime_SOURCE_DIR}/lib/src" PARENT_SCOPE)
  set(${ARG_OUT_SOURCES} "${grammar_sources}" PARENT_SCOPE)
  set(${ARG_OUT_INCLUDES} "" PARENT_SCOPE)
  set(${ARG_OUT_GENERATED} "${generated_source}" PARENT_SCOPE)
  set(${ARG_OUT_SOURCE_MAP} "${source_map}" PARENT_SCOPE)
endfunction()
