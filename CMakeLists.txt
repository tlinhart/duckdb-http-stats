cmake_minimum_required(VERSION 3.5)

# Set extension name here.
set(TARGET_NAME http_stats)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include
                    duckdb-httpfs/src/include)

set(EXTENSION_SOURCES src/http_stats_extension.cpp
                      src/http_stats_state.cpp
                      src/http_stats_http_util.cpp
                      duckdb-httpfs/src/http_state.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
