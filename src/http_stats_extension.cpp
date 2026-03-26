#include "http_stats_extension.hpp"

#include "http_stats_http_util.hpp"
#include "http_stats_state.hpp"
#include "http_stats_silent_state.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

// Replace any existing HTTPFS state with our silent subclass.
// If HTTPFS was loaded before us, "http_state" already holds a real HTTPState.
// GetOrCreate would return that existing object (wrong vtable), so we must
// Remove first to ensure we install our SilentHTTPState.
static void InstallSilentHTTPState(ClientContext &context) {
	context.registered_state->Remove(SilentHTTPState::HTTPFS_STATE_KEY);
	context.registered_state->GetOrCreate<SilentHTTPState>(SilentHTTPState::HTTPFS_STATE_KEY);
}

// ExtensionCallback that:
// 1. Registers a HTTPStatsState on every new connection.
// 2. Installs SilentHTTPState to suppress HTTPFS profiling output.
// 3. Re-wraps the HTTPUtil after another extension (e.g. httpfs) replaces it.
class HTTPStatsExtensionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		context.registered_state->GetOrCreate<HTTPStatsState>(HTTPStatsState::STATE_KEY);
		InstallSilentHTTPState(context);
	}

	void OnExtensionLoaded(DatabaseInstance &db, const string &name) override {
		auto &config = DBConfig::GetConfig(db);
		auto &current = config.GetHTTPUtil();
		// If the current HTTPUtil is no longer our wrapper (e.g. httpfs replaced it),
		// re-wrap the new one so we continue intercepting HTTP requests.
		if (!StringUtil::StartsWith(current.GetName(), "HTTPStatsHTTPUtil")) {
			auto wrapper = make_shared_ptr<HTTPStatsHTTPUtil>(current);
			config.SetHTTPUtil(wrapper);
		}
		// If HTTPFS just loaded, it may have registered its own HTTPState on
		// existing connections. Re-install our silent replacement on all of them.
		auto &conn_mgr = ConnectionManager::Get(db);
		for (auto &ctx : conn_mgr.GetConnectionList()) {
			InstallSilentHTTPState(*ctx);
		}
	}
};

void HttpStatsExtension::Load(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// Capture a reference to the currently installed HTTPUtil before we replace it.
	// SetHTTPUtil moves the old one into old_http_utils, so it stays alive.
	auto &original = config.GetHTTPUtil();
	auto wrapper = make_shared_ptr<HTTPStatsHTTPUtil>(original);
	config.SetHTTPUtil(wrapper);

	// Register our callback so that every new connection gets a HTTPStatsState.
	ExtensionCallback::Register(config, make_shared_ptr<HTTPStatsExtensionCallback>());

	// Also register HTTPStatsState and SilentHTTPState on all already-open connections
	// (including the one that issued LOAD), since OnConnectionOpened only fires for
	// connections created after this point.
	auto &conn_mgr = ConnectionManager::Get(db);
	for (auto &ctx : conn_mgr.GetConnectionList()) {
		ctx->registered_state->GetOrCreate<HTTPStatsState>(HTTPStatsState::STATE_KEY);
		InstallSilentHTTPState(*ctx);
	}

	loader.SetDescription("Intercepts HTTP requests and reports network stats in EXPLAIN ANALYZE output");
}

std::string HttpStatsExtension::Name() {
	return "http_stats";
}

std::string HttpStatsExtension::Version() const {
#ifdef EXT_VERSION_HTTP_STATS
	return EXT_VERSION_HTTP_STATS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(http_stats, loader) {
	duckdb::HttpStatsExtension extension;
	extension.Load(loader);
}
}
