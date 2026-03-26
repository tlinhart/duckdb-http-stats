#pragma once

#include "duckdb/common/http_util.hpp"

namespace duckdb {

// A wrapper around the original HTTPUtil that intercepts every HTTP request
// to collect statistics (request counts, bytes, time) into the active
// HTTPStatsState (found via thread-local storage).
//
// The original HTTPUtil is kept alive by DuckDB's DBConfig::old_http_utils
// mechanism (SetHTTPUtil moves the previous one there), so a raw reference
// is safe for the lifetime of the database.
class HTTPStatsHTTPUtil : public HTTPUtil {
public:
	explicit HTTPStatsHTTPUtil(HTTPUtil &original);

	string GetName() const override;

	unique_ptr<HTTPParams> InitializeParameters(DatabaseInstance &db, const string &path) override;
	unique_ptr<HTTPParams> InitializeParameters(ClientContext &context, const string &path) override;
	unique_ptr<HTTPParams> InitializeParameters(optional_ptr<FileOpener> opener,
	                                            optional_ptr<FileOpenerInfo> info) override;

	unique_ptr<HTTPClient> InitializeClient(HTTPParams &http_params, const string &proto_host_port) override;

	unique_ptr<HTTPResponse> SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) override;

	void LogRequest(BaseRequest &request, optional_ptr<HTTPResponse> response) override;

private:
	HTTPUtil &original_;
};

} // namespace duckdb
