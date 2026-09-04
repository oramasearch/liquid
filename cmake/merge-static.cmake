set(merged "${LIQUID_ARCHIVE}.merged")
file(REMOVE "${merged}")

if(HOST_APPLE)
  execute_process(
    COMMAND /usr/bin/libtool -static -o "${merged}"
            "${LIQUID_ARCHIVE}" "${ROCKSDB_ARCHIVE}" "${ZSTD_ARCHIVE}"
    RESULT_VARIABLE status)
elseif(HOST_MSVC)
  execute_process(
    COMMAND "${ARCHIVER}" /NOLOGO "/OUT:${merged}"
            "${LIQUID_ARCHIVE}" "${ROCKSDB_ARCHIVE}" "${ZSTD_ARCHIVE}"
    RESULT_VARIABLE status)
else()
  set(script "${LIQUID_ARCHIVE}.mri")
  file(WRITE "${script}"
    "CREATE ${merged}\nADDLIB ${LIQUID_ARCHIVE}\nADDLIB ${ROCKSDB_ARCHIVE}\nADDLIB ${ZSTD_ARCHIVE}\nSAVE\nEND\n")
  execute_process(
    COMMAND "${ARCHIVER}" -M
    INPUT_FILE "${script}"
    RESULT_VARIABLE status)
  file(REMOVE "${script}")
endif()

if(NOT status EQUAL 0)
  message(FATAL_ERROR "failed to merge libliquid's private static dependencies")
endif()

file(RENAME "${merged}" "${LIQUID_ARCHIVE}")
