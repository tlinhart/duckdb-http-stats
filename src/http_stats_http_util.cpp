#include "http_stats_http_util.hpp"
#include "http_stats_state.hpp"

#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

// Rebind the HTTPParams::http_util reference to point to new_target instead of old_target.
// Since http_util is a C++ reference (stored as a hidden pointer), we cannot reassign it
// directly. Instead, we scan the struct's raw memory for the old pointer value and replace it.
static void RebindHTTPUtilRef(HTTPParams &params, HTTPUtil &old_target, HTTPUtil &new_target) {
	auto old_val = reinterpret_cast<uintptr_t>(&old_target);
	auto new_val = reinterpret_cast<uintptr_t>(&new_target);
	auto *slots = reinterpret_cast<uintptr_t *>(&params);
	size_t count = sizeof(HTTPParams) / sizeof(uintptr_t);
	for (size_t i = 0; i < count; i++) {
		if (slots[i] == old_val) {
			slots[i] = new_val;
			return;
		}
	}
}

HTTPStatsHTTPUtil::HTTPStatsHTTPUtil(HTTPUtil &original) : original_(original) {
}

string HTTPStatsHTTPUtil::GetName() const {
	return "HTTPStatsHTTPUtil (wrapping " + original_.GetName() + ")";
}

unique_ptr<HTTPParams> HTTPStatsHTTPUtil::InitializeParameters(DatabaseInstance &db, const string &path) {
	auto params = original_.InitializeParameters(db, path);
	RebindHTTPUtilRef(*params, original_, *this);
	return params;
}

unique_ptr<HTTPParams> HTTPStatsHTTPUtil::InitializeParameters(ClientContext &context, const string &path) {
	auto params = original_.InitializeParameters(context, path);
	RebindHTTPUtilRef(*params, original_, *this);
	return params;
}

unique_ptr<HTTPParams> HTTPStatsHTTPUtil::InitializeParameters(optional_ptr<FileOpener> opener,
                                                               optional_ptr<FileOpenerInfo> info) {
	auto params = original_.InitializeParameters(opener, info);
	RebindHTTPUtilRef(*params, original_, *this);
	return params;
}

unique_ptr<HTTPClient> HTTPStatsHTTPUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	return original_.InitializeClient(http_params, proto_host_port);
}

unique_ptr<HTTPResponse> HTTPStatsHTTPUtil::SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) {
	auto *state = GetActiveHTTPStatsState();

	// If no active state (e.g. HTTP request outside a query context), just pass through.
	if (!state) {
		return original_.SendRequest(request, client);
	}

	// For GET requests with a content_handler (streaming), we need to wrap the handler
	// to count bytes as they flow through, since the response body will be empty.
	uint64_t streamed_bytes = 0;
	bool is_streaming_get = false;
	std::function<bool(const_data_ptr_t data, idx_t data_length)> original_content_handler;

	if (request.type == RequestType::GET_REQUEST) {
		auto &get_request = request.Cast<GetRequestInfo>();
		if (get_request.content_handler) {
			is_streaming_get = true;
			original_content_handler = get_request.content_handler;
			get_request.content_handler = [&streamed_bytes, &original_content_handler](const_data_ptr_t data,
			                                                                           idx_t data_length) -> bool {
				streamed_bytes += data_length;
				return original_content_handler(data, data_length);
			};
		}
	}

	// Capture timing around the request.
	auto start = Timestamp::GetCurrentTimestamp();
	auto response = original_.SendRequest(request, client);
	auto end = Timestamp::GetCurrentTimestamp();
	auto time_ms = Timestamp::GetEpochMs(end) - Timestamp::GetEpochMs(start);

	// Record stats.
	state->RecordRequest(request, response.get(), time_ms);

	// If we intercepted a streaming GET, add the streamed bytes to the stats
	// only when the response body is empty. The httpfs curl backend buffers
	// the entire body into response->body before invoking the content_handler,
	// so RecordRequest already counted those bytes via body.size(). Adding
	// streamed_bytes on top would double-count.
	if (is_streaming_get && streamed_bytes > 0 && (!response || response->body.empty())) {
		auto &h = state->GetHostStats(request.proto_host_port);
		h.bytes_received.fetch_add(streamed_bytes, std::memory_order_relaxed);
	}

	// Restore the original content handler.
	if (is_streaming_get) {
		request.Cast<GetRequestInfo>().content_handler = std::move(original_content_handler);
	}

	return response;
}

void HTTPStatsHTTPUtil::LogRequest(BaseRequest &request, optional_ptr<HTTPResponse> response) {
	original_.LogRequest(request, response);
}

} // namespace duckdb
