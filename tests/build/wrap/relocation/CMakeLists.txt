cmake_minimum_required(VERSION 3.28)
project(liquid_relocation_fixture C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

add_library(relocation_fixture SHARED library.c)
set_target_properties(relocation_fixture PROPERTIES OUTPUT_NAME relocation_fixture)

add_executable(relocation-tool main.c)
target_link_libraries(relocation-tool PRIVATE relocation_fixture)

install(
  TARGETS relocation_fixture relocation-tool
  EXPORT relocationFixtureTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES relocation.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(relocation.pc.in relocation.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/relocation.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

configure_package_config_file(
  relocationConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/relocationConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/relocation)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/relocationConfig.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/relocation)
install(EXPORT relocationFixtureTargets
  NAMESPACE relocation::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/relocation)
