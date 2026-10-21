# libFuzzer harnesses for the parsers of untrusted bytes (liquid #407). Configured only with
# -DLIQUID_FUZZ=ON, which also builds every target with AddressSanitizer, UndefinedBehaviorSanitizer
# and coverage instrumentation (cmake/Hardening.cmake). How to run them: docs/build/hardening.md.
#
#   cmake --build build --target liquid-fuzz         # every harness, plus the seed corpus
#   build/fuzz/liquid-fuzz-cbor build/fuzz/corpus/cbor -max_total_time=600

set(LIQUID_FUZZ_HARNESSES
  cbor
  configuration
  drip
  git
  http1
  http2
  identity
  objects
  protobuf
  resolve
  sync)

set(LIQUID_FUZZ_CORPUS_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/corpus")

add_executable(liquid-fuzz-seeds seeds.cpp)
target_include_directories(liquid-fuzz-seeds PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(liquid-fuzz-seeds PRIVATE liquid::liquid)

add_custom_target(liquid-fuzz-corpus
  COMMAND liquid-fuzz-seeds ${PROJECT_SOURCE_DIR} ${LIQUID_FUZZ_CORPUS_DIRECTORY}
  COMMENT "Writing the fuzz seed corpus to ${LIQUID_FUZZ_CORPUS_DIRECTORY}"
  VERBATIM)

add_custom_target(liquid-fuzz)
add_dependencies(liquid-fuzz liquid-fuzz-corpus)

foreach(LIQUID_FUZZ_HARNESS IN LISTS LIQUID_FUZZ_HARNESSES)
  set(LIQUID_FUZZ_TARGET liquid-fuzz-${LIQUID_FUZZ_HARNESS})
  add_executable(${LIQUID_FUZZ_TARGET} ${LIQUID_FUZZ_HARNESS}.cpp options.cpp)
  target_include_directories(${LIQUID_FUZZ_TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${LIQUID_FUZZ_TARGET} PRIVATE liquid::liquid)
  target_link_options(${LIQUID_FUZZ_TARGET} PRIVATE -fsanitize=fuzzer)
  add_dependencies(liquid-fuzz ${LIQUID_FUZZ_TARGET})
endforeach()

# The HTTP/1 harness drives Boost.HTTP's request parser directly, and the drip harness includes
# the CRDT journal, whose header names automerge-cpp types.
target_link_libraries(liquid-fuzz-http1 PRIVATE Boost::http)
target_link_libraries(liquid-fuzz-drip PRIVATE automerge-cpp::automerge-cpp)
