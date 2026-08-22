//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/fqp_optimizer_internal.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/join_order/query_graph.hpp"

namespace duckdb {

class QueryGraphManager;

namespace fqp_internal {

struct ExplainCost {
	ExplainCost() : startup_cost(0), total_cost(0), rows(0), width(0) {
	}

	ExplainCost(double startup_cost, double total_cost, idx_t rows, int width)
	    : startup_cost(startup_cost), total_cost(total_cost), rows(rows), width(width) {
	}

	double startup_cost;
	double total_cost;
	idx_t rows;
	int width;
};

struct RemoteConfig {
	bool enabled = false;
	string source_id;
	string host;
	int base_port = 15433;
	int timeout_ms = 20000;
};

struct RelationInfo {
	string table_name;
	string schema_name;
	string source;
	bool remote = false;
};

string QuoteIdentifier(const string &input);
bool ParseSourcePrefix(const string &relname, string &source);
int InterfacePortForSource(const string &source, int base_port);
string JsonEscape(const string &input);
bool ExtractJSONNumber(const string &json, const string &key, double &result);

bool RemoteExplainHTTP(const RemoteConfig &config, const string &target_source, const string &probe_kind,
                       const string &sql, ExplainCost &cost);

bool GetRelationInfo(QueryGraphManager &query_graph_manager, idx_t relation_id, RelationInfo &result);
bool DeparseJoinCandidate(QueryGraphManager &query_graph_manager, JoinRelationSet &set, const string &dest_source,
                          const string &local_schema, string &sql);
bool DeparseBaseCandidate(QueryGraphManager &query_graph_manager, idx_t relation_id, const string &dest_source,
                          const string &local_schema, string &sql);

} // namespace fqp_internal
} // namespace duckdb
