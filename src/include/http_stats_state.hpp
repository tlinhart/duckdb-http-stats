#pragma once

#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace duckdb {

// Per-host HTTP statistics.
struct HostStats {
	std::atomic<uint64_t> head_count {0};
	std::atomic<uint64_t> get_count {0};
	std::atomic<uint64_t> put_count {0};
	std::atomic<uint64_t> post_count {0};
	std::atomic<uint64_t> delete_count {0};
	std::atomic<uint64_t> bytes_sent {0};
	std::atomic<uint64_t> bytes_received {0};
	std::atomic<int64_t> total_time_ms {0};

	uint64_t TotalRequests() const;
};

// Per-connection query-level HTTP statistics. Registered as a ClientContextState
// so that it hooks into QueryBegin (to reset) and WriteProfilingInformation
// (to emit stats in EXPLAIN ANALYZE output).
class HTTPStatsState : public ClientContextState {
public:
	static constexpr const char *STATE_KEY = "http_stats";

	HTTPStatsState() = default;

	// ClientContextState hooks.
	void QueryBegin(ClientContext &context) override;
	void WriteProfilingInformation(std::ostream &ss) override;
	void OnTaskStart(ClientContext &context) override;
	void OnTaskStop(ClientContext &context) override;

	// Stats recording (called from HTTPStatsHTTPUtil).
	void RecordRequest(const BaseRequest &request, const HTTPResponse *response, int64_t time_ms);

	// Per-host stats accessor.
	HostStats &GetHostStats(const string &proto_host_port);

private:
	// Per-host breakdown. Protected by mutex because new hosts can be inserted.
	mutex host_lock_;
	unordered_map<string, unique_ptr<HostStats>> per_host_stats_;

	void Reset();
};

// Thread-local pointer to the active HTTPStatsState. This is used by
// HTTPStatsHTTPUtil::SendRequest to find the correct per-connection state
// without needing a ClientContext reference (which HTTPUtil doesn't receive).
HTTPStatsState *GetActiveHTTPStatsState();
void SetActiveHTTPStatsState(HTTPStatsState *state);

} // namespace duckdb
