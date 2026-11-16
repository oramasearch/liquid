# Every dependency the build fetches is pinned to its content (liquid#408): a URL carries URL_HASH,
# a git source a 40-hex commit. A tag or branch alone can move upstream.
file(READ "${LIQUID_SOURCE_DIR}/CMakeLists.txt" contents)
string(REGEX MATCHALL "FetchContent_Declare\\([^)]*\\)" declarations "${contents}")
foreach(declaration IN LISTS declarations)
  if(declaration MATCHES "GIT_REPOSITORY")
    if(NOT declaration MATCHES "GIT_TAG +[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]")
      message(FATAL_ERROR "a git dependency is not pinned to a commit: ${declaration}")
    endif()
  elseif(declaration MATCHES "URL " AND NOT declaration MATCHES "URL_HASH")
    message(FATAL_ERROR "a URL dependency has no URL_HASH: ${declaration}")
  endif()
endforeach()
