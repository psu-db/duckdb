//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/fqp_optimizer.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/query_graph.hpp"

namespace duckdb {

class QueryGraphManager;
class ClientContext;
class LogicalOperator;

class FQPOptimizer {
public:
	static void Clear(ClientContext &context);
	static void SetMockCost(ClientContext &context, const string &source, const string &sql, double startup_cost,
	                        double total_cost, int64_t rows, int width);
	static void SetHTTPConfig(ClientContext &context, const string &source_id, const string &host, int base_port,
	                          int timeout_ms);
	static void SetHTTPEnabled(ClientContext &context, bool enabled);
	static void SetDataMovementFactor(ClientContext &context, double factor);
	static void SetRemoteSchema(ClientContext &context, const string &schema);
	static string LastDeparse(ClientContext &context);

	static bool TryGetBaseCost(QueryGraphManager &query_graph_manager, idx_t relation_id, idx_t fallback_rows,
	                           FQPPlanAlternative &result);
	static vector<FQPPlanAlternative> GetJoinAlternatives(QueryGraphManager &query_graph_manager, JoinRelationSet &set,
	                                                      DPJoinNode &left, DPJoinNode &right,
	                                                      const vector<reference<NeighborInfo>> &possible_connections);
	static unique_ptr<LogicalOperator> WrapPlan(unique_ptr<LogicalOperator> plan, const DPJoinNode &node);
};

} // namespace duckdb
