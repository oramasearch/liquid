# Every image's --control-listen is a loopback address (liquid#393): the control APIs have no
# authentication, so an image must never listen on them off loopback.
file(GLOB dockerfiles "${LIQUID_SOURCE_DIR}/deploy/*/Dockerfile")
foreach(dockerfile IN LISTS dockerfiles)
  file(READ "${dockerfile}" contents)
  string(REGEX MATCHALL "\"--control-listen\", *\"[^\"]*\"" listeners "${contents}")
  foreach(listener IN LISTS listeners)
    if(NOT listener MATCHES "\"127\\.0\\.0\\.1:[0-9]+\"$" AND NOT listener MATCHES "\"\\[::1\\]:[0-9]+\"$")
      message(FATAL_ERROR "${dockerfile}: control listener off loopback: ${listener}")
    endif()
  endforeach()
endforeach()
