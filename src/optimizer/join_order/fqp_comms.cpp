#include "duckdb/optimizer/join_order/fqp_optimizer_internal.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace duckdb {
namespace fqp_internal {

#ifndef _WIN32
static bool SendAll(int fd, const string &payload) {
	idx_t offset = 0;
	while (offset < payload.size()) {
		int flags = 0;
#ifdef MSG_NOSIGNAL
		flags = MSG_NOSIGNAL;
#endif
		auto written = send(fd, payload.data() + offset, payload.size() - offset, flags);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return false;
		}
		offset += idx_t(written);
	}
	return true;
}

static bool HTTPPostJSON(const string &host, int port, int timeout_ms, const string &path, const string &body,
                         string &response_body) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *addresses = nullptr;
	auto port_string = to_string(port);
	if (getaddrinfo(host.c_str(), port_string.c_str(), &hints, &addresses) != 0) {
		return false;
	}

	int fd = -1;
	for (auto address = addresses; address != nullptr; address = address->ai_next) {
		fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (fd < 0) {
			continue;
		}
		struct timeval timeout;
		timeout.tv_sec = timeout_ms / 1000;
		timeout.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
		int no_sigpipe = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
		if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(addresses);
	if (fd < 0) {
		return false;
	}

	string request = "POST " + path + " HTTP/1.1\r\n";
	request += "Host: " + host + ":" + to_string(port) + "\r\n";
	request += "Content-Type: application/json\r\n";
	request += "Connection: close\r\n";
	request += "Content-Length: " + to_string(body.size()) + "\r\n\r\n";
	request += body;

	if (!SendAll(fd, request)) {
		close(fd);
		return false;
	}

	string response;
	char buffer[4096];
	while (true) {
		auto read_count = recv(fd, buffer, sizeof(buffer), 0);
		if (read_count < 0 && errno == EINTR) {
			continue;
		}
		if (read_count < 0) {
			close(fd);
			return false;
		}
		if (read_count == 0) {
			break;
		}
		response.append(buffer, idx_t(read_count));
	}
	close(fd);

	if (response.find("HTTP/1.1 200") != 0 && response.find("HTTP/1.0 200") != 0) {
		return false;
	}
	auto body_pos = response.find("\r\n\r\n");
	if (body_pos == string::npos) {
		return false;
	}
	response_body = response.substr(body_pos + 4);
	return true;
}
#endif

bool RemoteExplainHTTP(const RemoteConfig &config, const string &target_source, const string &probe_kind,
                       const string &sql, ExplainCost &cost) {
	if (!config.enabled || config.source_id.empty() || target_source.empty()) {
		return false;
	}

#ifdef _WIN32
	return false;
#else
	auto body = "{\"version\":1,\"source_id\":" + JsonEscape(config.source_id) +
	            ",\"target_source_id\":" + JsonEscape(target_source) + ",\"probe_kind\":" + JsonEscape(probe_kind) +
	            ",\"sql_text\":" + JsonEscape(sql) + ",\"timeout_ms\":" + to_string(config.timeout_ms) + "}";
	string response;
	if (!HTTPPostJSON(config.host, InterfacePortForSource(config.source_id, config.base_port), config.timeout_ms,
	                  "/v1/plan/explain/relay", body, response)) {
		return false;
	}
	double startup = 0;
	double total = 0;
	double rows = 0;
	double width = 0;
	if (!ExtractJSONNumber(response, "startup_cost", startup) || !ExtractJSONNumber(response, "total_cost", total)) {
		return false;
	}
	(void)ExtractJSONNumber(response, "plan_rows", rows);
	(void)ExtractJSONNumber(response, "plan_width", width);
	cost.startup_cost = startup;
	cost.total_cost = total;
	cost.rows = std::isfinite(rows) && rows > 0 && rows <= double(std::numeric_limits<idx_t>::max()) ? idx_t(rows) : 1;
	cost.width = std::isfinite(width) && width > 0 && width <= double(std::numeric_limits<int>::max()) ? int(width) : 0;
	return true;
#endif
}

} // namespace fqp_internal
} // namespace duckdb
