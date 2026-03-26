# This file is included by DuckDB's build system. It specifies which extensions
# to load.

# Extension from this repo.
duckdb_extension_load(http_stats
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
