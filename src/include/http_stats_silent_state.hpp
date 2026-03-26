#pragma once

#include "http_state.hpp"

namespace duckdb {

// Subclass of HTTPFS's HTTPState that suppresses its profiling output in
// EXPLAIN ANALYZE. All HTTPState functionality (counters, cache, Reset(),
// QueryEnd()) is inherited and works normally - the only override is
// WriteProfilingInformation which produces no output. Our own HTTPStatsState
// handles profiling output instead.
class SilentHTTPState : public HTTPState {
public:
	static constexpr const char *HTTPFS_STATE_KEY = "http_state";

	// Suppress HTTPFS profiling output: our HTTPStatsState handles the output.
	void WriteProfilingInformation(std::ostream &) override {
	}
};

} // namespace duckdb
