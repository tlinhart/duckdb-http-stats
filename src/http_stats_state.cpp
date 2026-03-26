#include "http_stats_state.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

static thread_local HTTPStatsState *active_http_stats_state = nullptr;

HTTPStatsState *GetActiveHTTPStatsState() {
	return active_http_stats_state;
}

void SetActiveHTTPStatsState(HTTPStatsState *state) {
	active_http_stats_state = state;
}

uint64_t HostStats::TotalRequests() const {
	return head_count.load(std::memory_order_relaxed) + get_count.load(std::memory_order_relaxed) +
	       put_count.load(std::memory_order_relaxed) + post_count.load(std::memory_order_relaxed) +
	       delete_count.load(std::memory_order_relaxed);
}

void HTTPStatsState::Reset() {
	lock_guard<mutex> lk(host_lock_);
	per_host_stats_.clear();
}

void HTTPStatsState::QueryBegin(ClientContext &context) {
	Reset();
	// Set this state as the active one for the current thread (the main query thread).
	SetActiveHTTPStatsState(this);
}

void HTTPStatsState::OnTaskStart(ClientContext &context) {
	// Propagate the thread-local pointer to worker threads so that
	// HTTPStatsHTTPUtil::SendRequest can find the active state.
	SetActiveHTTPStatsState(this);
}

void HTTPStatsState::OnTaskStop(ClientContext &context) {
	// Clear the thread-local pointer when the task finishes on this worker thread.
	SetActiveHTTPStatsState(nullptr);
}

HostStats &HTTPStatsState::GetHostStats(const string &proto_host_port) {
	lock_guard<mutex> lk(host_lock_);
	auto it = per_host_stats_.find(proto_host_port);
	if (it != per_host_stats_.end()) {
		return *it->second;
	}
	auto stats = make_uniq<HostStats>();
	auto &ref = *stats;
	per_host_stats_.emplace(proto_host_port, std::move(stats));
	return ref;
}

void HTTPStatsState::RecordRequest(const BaseRequest &request, const HTTPResponse *response, int64_t time_ms) {
	// Determine bytes sent (for PUT/POST the request body size).
	uint64_t sent = 0;
	if (request.type == RequestType::PUT_REQUEST) {
		sent = request.Cast<const PutRequestInfo>().buffer_in_len;
	} else if (request.type == RequestType::POST_REQUEST) {
		sent = request.Cast<const PostRequestInfo>().buffer_in_len;
	}

	// Determine bytes received from the response body.
	// Note: for streaming GETs where body is empty, bytes are counted separately
	// via the streamed_bytes wrapper in HTTPStatsHTTPUtil::SendRequest().
	uint64_t received = 0;
	if (response) {
		received = response->body.size();
	}

	// Update per-host stats (keyed by proto_host_port e.g. "https://s3.amazonaws.com").
	auto &h = GetHostStats(request.proto_host_port);
	switch (request.type) {
	case RequestType::HEAD_REQUEST:
		h.head_count.fetch_add(1, std::memory_order_relaxed);
		break;
	case RequestType::GET_REQUEST:
		h.get_count.fetch_add(1, std::memory_order_relaxed);
		break;
	case RequestType::PUT_REQUEST:
		h.put_count.fetch_add(1, std::memory_order_relaxed);
		break;
	case RequestType::POST_REQUEST:
		h.post_count.fetch_add(1, std::memory_order_relaxed);
		break;
	case RequestType::DELETE_REQUEST:
		h.delete_count.fetch_add(1, std::memory_order_relaxed);
		break;
	}
	h.bytes_sent.fetch_add(sent, std::memory_order_relaxed);
	h.bytes_received.fetch_add(received, std::memory_order_relaxed);
	h.total_time_ms.fetch_add(time_ms, std::memory_order_relaxed);
}

static string FormatRequestTypes(const HostStats &stats) {
	string result;
	auto head = stats.head_count.load(std::memory_order_relaxed);
	auto get = stats.get_count.load(std::memory_order_relaxed);
	auto put = stats.put_count.load(std::memory_order_relaxed);
	auto post = stats.post_count.load(std::memory_order_relaxed);
	auto del = stats.delete_count.load(std::memory_order_relaxed);
	bool first = true;
	auto append = [&](const char *label, uint64_t count) {
		if (count == 0) {
			return;
		}
		if (!first) {
			result += "  ";
		}
		result += string(label) + ": " + to_string(count);
		first = false;
	};
	append("HEAD", head);
	append("GET", get);
	append("PUT", put);
	append("POST", post);
	append("DELETE", del);
	return result;
}

static string StripProtocol(const string &host) {
	if (StringUtil::StartsWith(host, "https://")) {
		return host.substr(8);
	}
	if (StringUtil::StartsWith(host, "http://")) {
		return host.substr(7);
	}
	return host;
}

// Repeat a (possibly multi-byte) UTF-8 string N times.
static string RepeatString(const string &s, idx_t count) {
	string result;
	result.reserve(s.size() * count);
	for (idx_t i = 0; i < count; i++) {
		result += s;
	}
	return result;
}

// Count the number of visible (display) characters in a UTF-8 string.
// UTF-8 continuation bytes (0x80..0xBF) are not counted since they are
// trailing bytes of multi-byte sequences, not separate characters.
static idx_t DisplayWidth(const string &s) {
	idx_t width = 0;
	for (auto c : s) {
		if ((c & 0xC0) != 0x80) {
			width++;
		}
	}
	return width;
}

// Center a string within the given width using its display width (visible
// character count) rather than byte length. This handles multi-byte UTF-8
// strings correctly, unlike QueryProfiler::DrawPadded which uses byte length.
static string PadCentered(const string &str, idx_t width) {
	auto dw = DisplayWidth(str);
	if (dw >= width) {
		return str;
	}
	auto remaining = width - dw;
	auto half = remaining / 2;
	auto extra = remaining % 2 != 0 ? 1 : 0;
	return string(half + extra, ' ') + str + string(half, ' ');
}

void HTTPStatsState::WriteProfilingInformation(std::ostream &ss) {
	lock_guard<mutex> lk(host_lock_);
	if (per_host_stats_.empty()) {
		return;
	}

	// Collect all content lines first so we can compute the box width.
	vector<string> lines;

	lines.push_back("HTTP Stats");
	lines.push_back("");

	// Per-host sections.
	bool first_host = true;
	for (auto &pair : per_host_stats_) {
		auto &hs = *pair.second;
		auto h_sent = hs.bytes_sent.load(std::memory_order_relaxed);
		auto h_recv = hs.bytes_received.load(std::memory_order_relaxed);
		auto h_time = hs.total_time_ms.load(std::memory_order_relaxed);
		auto host_name = StripProtocol(pair.first);

		if (!first_host) {
			lines.push_back("");
		}
		first_host = false;

		lines.push_back(host_name);
		lines.push_back(RepeatString("\u2500", host_name.size())); // ─ separator
		lines.push_back("Total Requests: " + to_string(hs.TotalRequests()));
		lines.push_back(FormatRequestTypes(hs));
		lines.push_back("in: " + StringUtil::BytesToHumanReadableString(h_recv) +
		                "  out: " + StringUtil::BytesToHumanReadableString(h_sent));
		lines.push_back("Total Time: " + StringUtil::Format("%.2f", h_time / 1000.0) + "s");
	}

	// Compute dynamic box width from the longest content line.
	constexpr idx_t MIN_INNER_WIDTH = 35; // matches HTTPFS's default inner width
	constexpr idx_t CONTENT_PADDING = 4;  // 2 spaces of breathing room on each side
	idx_t max_content = 0;
	for (auto &line : lines) {
		max_content = MaxValue<idx_t>(max_content, DisplayWidth(line));
	}
	idx_t inner_width = MaxValue<idx_t>(max_content + CONTENT_PADDING, MIN_INNER_WIDTH);
	// Ensure even inner width so PadCentered centers symmetrically.
	if (inner_width % 2 != 0) {
		inner_width++;
	}

	// Render box with dynamic borders.
	string outer_border = RepeatString("\u2500", inner_width + 2); // ─
	string inner_border = RepeatString("\u2500", inner_width);     // ─

	ss << "\u250C" << outer_border << "\u2510\n";             // ┌───┐
	ss << "\u2502\u250C" << inner_border << "\u2510\u2502\n"; // │┌─┐│
	for (auto &line : lines) {
		ss << "\u2502\u2502" << PadCentered(line, inner_width) << "\u2502\u2502\n";
	}
	ss << "\u2502\u2514" << inner_border << "\u2518\u2502\n"; // │└─┘│
	ss << "\u2514" << outer_border << "\u2518\n";             // └───┘
}

} // namespace duckdb
