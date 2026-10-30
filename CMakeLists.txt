cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_VER)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Keep the checked-in lock portable. IDF serializes local sources as absolute
# paths, so only its build-directory copy may be rewritten by the resolver.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock" YA_LOCK)
string(REPLACE "path: components/" "path: ${CMAKE_CURRENT_SOURCE_DIR}/components/" YA_LOCK "${YA_LOCK}")
file(WRITE "${CMAKE_BINARY_DIR}/dependencies.lock" "${YA_LOCK}")
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
project(waveshare-ya)
