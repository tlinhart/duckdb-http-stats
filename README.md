# DuckDB HTTP Stats Extension

## Overview

The HTTP stats extension for DuckDB transparently intercepts all HTTP traffic
and collects request statistics (request counts, bytes sent and received and
request time) on a per-host basis. The collected statistics are surfaced
through DuckDB's `EXPLAIN ANALYZE` output:

```
┌──────────────────────────────────────┐
│┌────────────────────────────────────┐│
││             HTTP Stats             ││
││                                    ││
││   community-extensions.duckdb.org  ││
││   ───────────────────────────────  ││
││          Total Requests: 2         ││
││           HEAD: 1  GET: 1          ││
││      in: 3.0 KiB  out: 0 bytes     ││
││          Total Time: 0.45s         ││
││                                    ││
││    s3.eu-central-1.amazonaws.com   ││
││    ─────────────────────────────   ││
││          Total Requests: 2         ││
││           HEAD: 1  GET: 1          ││
││      in: 1.8 KiB  out: 0 bytes     ││
││          Total Time: 0.37s         ││
│└────────────────────────────────────┘│
└──────────────────────────────────────┘
```

The extension works alongside the
[HTTPFS](https://github.com/duckdb/duckdb-httpfs) extension, which provides
DuckDB with HTTP(S) and S3 file system support using a curl-based HTTP backend.
When both extensions are loaded, HTTP stats wraps HTTPFS's HTTP backend to
measure traffic while HTTPFS handles the actual transport.

The motivation for building a separate stats extension rather than relying on
HTTPFS's built-in profiling output is twofold:

- HTTPFS shows empty counters when querying DuckDB databases stored on S3.
- HTTPFS does not provide per-host breakdown for queries that fetch data from
  multiple remote servers.

## How it works

DuckDB core defines an abstract HTTP backend (`HTTPUtil`) with a pluggable
replacement mechanism. By default, core ships with a minimal httplib-based
implementation that only supports basic GET requests (enough to download and
install extensions). The HTTPFS extension upgrades this by replacing the
built-in backend with a fully-featured curl-based implementation that adds
support for all HTTP methods, HTTPS, S3 and cloud storage.

The HTTP stats extension uses the decorator pattern to wrap whatever `HTTPUtil`
is currently active, whether that's core's built-in implementation or HTTPFS's
curl-based one. On load, it captures the active HTTP backend, creates a wrapper
around it and installs the wrapper as the new backend. The wrapper delegates
all operations to the original backend but intercepts requests to measure
statistics (count, bytes, time) per host.

To handle extension load order (e.g. HTTPFS loading after HTTP stats and
replacing the HTTP backend), the extension registers an `OnExtensionLoaded`
callback that detects when the active backend has been replaced and re-wraps
the new one. This ensures HTTP stats always remains the outermost wrapper.

When both extensions are loaded, DuckDB's profiler would normally render stats
from both HTTP stats and HTTPFS, leading to duplicate output. To prevent this,
HTTP stats replaces HTTPFS's profiling state with a silent subclass that
suppresses its output while preserving all other HTTPFS functionality.

```
┌─────────────────────────────────────────────────────┐
│  DuckDB core                                        │
│                                                     │
│  DBConfig                                           │
│    └─ shared_ptr<HTTPUtil> http_util ───────┐       │
│                                             │       │
│  HTTPUtil                                   │       │
│  (abstract base + built-in implementation)  │       │
│    • httplib-based, GET only                │       │
│    • Pluggable via Set/GetHTTPUtil          │       │
│                                             │       │
│  RegisteredStateManager                     │       │
│    ├─ http_stats → HTTPStatsState           │       │
│    │    └─ WriteProfilingInformation → our  │       │
│    │       stats box (or nothing if idle)   │       │
│    └─ http_state → SilentHTTPState          │       │
│         └─ WriteProfilingInformation → noop │       │
│         └─ Proper subclass of HTTPState     │       │
└─────────────────────────────────────────────┼───────┘
                                              │
              ┌───────────────────────────────┘
              ▼
┌───────────────────────────────────────────┐
│    HTTP stats extension (this project)    │
│                                           │
│    HTTPStatsHTTPUtil : public HTTPUtil    │
│     • Wraps the active HTTPUtil           │
│     • Intercepts SendRequest              │
│     • Measures stats (count, bytes, time) │
│       per host                            │
│     • Re-wraps on OnExtensionLoaded       │
└──────────────┬────────────────────────────┘
               │ delegates to
               ▼
┌──────────────────────────────────────────┐
│  Wrapped HTTPUtil                        │
│                                          │
│  Either:                                 │
│    • Core's built-in (httplib)           │
│    • HTTPFS's curl-based implementation  │
│    • Any other HTTPUtil subclass         │
└──────────────────────────────────────────┘
```

## Installation

The easiest way to install the HTTP stats extension is from the DuckDB
[community extensions](https://duckdb.org/community_extensions) repository:

```sql
INSTALL http_stats FROM community;
LOAD http_stats;
```

## Build from source

### Prerequisites

- C++11 compatible compiler
- CMake 3.5 or higher
- [vcpkg](https://vcpkg.io) (for dependency management)
- [Ninja](https://ninja-build.org) (recommended for parallelizing the build
  process)
- [ccache](https://ccache.dev) (recommended for caching compilation results and
  faster rebuilds)

### Clone the repository

```shell
git clone --recurse-submodules https://github.com/tlinhart/duckdb-http-stats.git
cd duckdb-http-stats
```

The `--recurse-submodules` flag is required to pull the DuckDB core, HTTPFS
extension and extension CI tools submodules. If you already cloned without
submodules:

```shell
git submodule update --init --recursive
```

### Set up vcpkg

The HTTP stats extension itself has no external dependencies, but it builds the
HTTPFS extension alongside it for testing, which requires OpenSSL and curl
managed through vcpkg. Set it up as follows:

```shell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
git checkout 84bab45d415d22042bd0b9081aea57f362da3f35
./bootstrap-vcpkg.sh -disableMetrics
export VCPKG_TOOLCHAIN_PATH=$(pwd)/scripts/buildsystems/vcpkg.cmake
```

The build system will automatically use vcpkg when `VCPKG_TOOLCHAIN_PATH` is
set. Dependencies are declared in `vcpkg.json`.

### Build with Make

The simplest way to build the extension is using Make:

```shell
make
```

This will create a release build of both the static and loadable extension.

DuckDB extensions build DuckDB itself as part of the process to provide easy
testing and distributing. The build process also compiles the HTTPFS extension
as a dependency. To speed up rebuilds significantly it's highly recommended to
install Ninja and ccache. The build system automatically detects and uses
ccache to cache build artifacts. To parallelize builds using Ninja:

```shell
GEN=ninja make
```

To limit the number of parallel jobs (if running low on memory):

```shell
CMAKE_BUILD_PARALLEL_LEVEL=4 GEN=ninja make
```

The main binaries produced by the build are:

- `build/release/duckdb` – DuckDB shell with extension pre-loaded.
- `build/release/test/unittest` – test runner with extension linked into the
  binary.
- `build/release/extension/http_stats/http_stats.duckdb_extension` –
  loadable extension binary as it would be distributed.

### Run the tests

The HTTP stats extension is equipped with a test suite under the `test`
directory. To run tests after the build:

```shell
make test
```

### Load the extension

To run the extension code, simply start the built shell with pre-loaded
extension:

```shell
./build/release/duckdb
```

Alternatively, start the DuckDB shell with `-unsigned` flag and load the
extension manually:

```sql
LOAD 'build/release/extension/http_stats/http_stats.duckdb_extension';
```

## License

See the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! This extension tries to follow the conventions and
guidelines used by the DuckDB project and its extension ecosystem.

The general process is as follows:

1. Fork the repository.
1. Create a feature branch.
1. Make your changes, including tests and documentation updates.
1. [Build the extension](#build-with-make).
1. [Run tests](#run-the-tests).
1. Run the linter (`make tidy-check`) and formatter (`make format-fix`).
1. Commit the changes.
1. Push to your fork and submit a pull request.

When submitting a pull request:

- Keep PRs focused and reasonably sized; large PRs are harder to review.
- Clearly describe the problem and solution in the PR description.
- Reference any related issues.
- Ensure all CI checks pass.
- Avoid draft PRs; use issues or discussions for work-in-progress ideas.

For major changes, please open an issue first to discuss the proposed changes.
