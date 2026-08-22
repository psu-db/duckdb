#include "duckdb/optimizer/join_order/fqp_optimizer_internal.hpp"

#include "duckdb/common/string_util.hpp"

#include <cstdlib>

namespace duckdb {
namespace fqp_internal {

string QuoteIdentifier(const string &input) {
	string result = "\"";
	for (auto c : input) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

bool ParseSourcePrefix(const string &relname, string &source) {
	auto sep = relname.find('_');
	if (sep == string::npos || sep == 0) {
		return false;
	}
	source = relname.substr(0, sep);
	return true;
}

int InterfacePortForSource(const string &source, int base_port) {
	if (source.size() <= 2 || source[0] != 'p' || source[1] != 'g') {
		return base_port;
	}
	char *endptr = nullptr;
	auto parsed = std::strtol(source.c_str() + 2, &endptr, 10);
	if (endptr == source.c_str() + 2 || *endptr != '\0') {
		return base_port;
	}
	return base_port + int(parsed) - 1;
}

string JsonEscape(const string &input) {
	string result = "\"";
	for (auto c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				result += StringUtil::Format("\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
			} else {
				result += c;
			}
			break;
		}
	}
	result += "\"";
	return result;
}

bool ExtractJSONNumber(const string &json, const string &key, double &result) {
	auto key_pos = json.find("\"" + key + "\"");
	if (key_pos == string::npos) {
		return false;
	}
	auto colon = json.find(':', key_pos);
	if (colon == string::npos) {
		return false;
	}
	auto start = colon + 1;
	while (start < json.size() && StringUtil::CharacterIsSpace(json[start])) {
		start++;
	}
	char *endptr = nullptr;
	result = std::strtod(json.c_str() + start, &endptr);
	return endptr != json.c_str() + start;
}

} // namespace fqp_internal
} // namespace duckdb
