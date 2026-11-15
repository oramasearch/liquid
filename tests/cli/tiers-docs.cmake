# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_UNIVERSALITY_DOC OR
   NOT DEFINED LIQUID_FORMATS_DOC)
  message(FATAL_ERROR "tiers docs test arguments are required")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" tiers --markdown
  RESULT_VARIABLE tiers_result
  OUTPUT_VARIABLE tiers_output
  ERROR_VARIABLE tiers_error)
if(NOT tiers_result EQUAL 0)
  message(FATAL_ERROR "liquid tiers --markdown failed: ${tiers_error}")
endif()

file(READ "${LIQUID_UNIVERSALITY_DOC}" universality_doc)
file(READ "${LIQUID_FORMATS_DOC}" formats_doc)

function(extract_block output source begin_marker end_marker)
  set(contents "${${source}}")
  string(FIND "${contents}" "${begin_marker}" begin_position)
  string(FIND "${contents}" "${end_marker}" end_position)
  if(begin_position EQUAL -1 OR end_position EQUAL -1 OR end_position LESS begin_position)
    message(FATAL_ERROR "missing ${begin_marker} ... ${end_marker}")
  endif()
  string(LENGTH "${end_marker}" end_length)
  math(EXPR block_length "${end_position} + ${end_length} - ${begin_position}")
  string(SUBSTRING "${contents}" ${begin_position} ${block_length} block)
  set("${output}" "${block}" PARENT_SCOPE)
endfunction()

extract_block(generated_tiers tiers_output "<!-- tiers:begin -->" "<!-- tiers:end -->")
extract_block(documented_tiers universality_doc "<!-- tiers:begin -->" "<!-- tiers:end -->")
if(NOT generated_tiers STREQUAL documented_tiers)
  message(FATAL_ERROR "universality tiers block differs from liquid tiers --markdown")
endif()

extract_block(generated_formats tiers_output "<!-- formats:begin -->" "<!-- formats:end -->")
extract_block(documented_formats formats_doc "<!-- formats:begin -->" "<!-- formats:end -->")
if(NOT generated_formats STREQUAL documented_formats)
  message(FATAL_ERROR "format tiers block differs from liquid tiers --markdown")
endif()
