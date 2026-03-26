# This file is included by DuckDB's build system. It specifies which extensions
# to load.

# Extension from this repo.
duckdb_extension_load(http_stats
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# HTTPFS extension and its dependencies (needed for tests that make HTTP requests).
duckdb_extension_load(json)
duckdb_extension_load(parquet)
duckdb_extension_load(httpfs
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/duckdb-httpfs
)
