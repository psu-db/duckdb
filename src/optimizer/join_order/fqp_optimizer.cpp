#include "duckdb/optimizer/join_order/fqp_optimizer.hpp"
#include "duckdb/optimizer/join_order/fqp_optimizer_internal.hpp"

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace duckdb {

using namespace fqp_internal;

struct FQPMockCostKey {
	string source;
	string sql;

	bool operator<(const FQPMockCostKey &other) const {
		if (source != other.source) {
			return source < other.source;
		}
		return sql < other.sql;
	}
};

struct FQPState : public ClientContextState {
	std::mutex lock;
	bool http_enabled = false;
	string source_id;
	string interface_host = "127.0.0.1";
	int interface_base_port = 15433;
	int timeout_ms = 20000;
	double data_movement_factor = 100;
	string local_schema = "public";
	string last_deparse;
	std::map<FQPMockCostKey, ExplainCost> mock_costs;
};

static FQPState &GetFQPState(ClientContext &context) {
	return *context.registered_state->GetOrCreate<FQPState>("fqp_optimizer");
}

static RemoteConfig GetRemoteConfig(ClientContext &context) {
	RemoteConfig config;
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	config.enabled = state.http_enabled;
	config.source_id = state.source_id;
	config.host = state.interface_host;
	config.base_port = state.interface_base_port;
	config.timeout_ms = state.timeout_ms;
	return config;
}

static void AddCandidateSources(const DPJoinNode &node, vector<string> &sources) {
	for (auto &alternative : node.fqp_alternatives) {
		if (std::find(sources.begin(), sources.end(), alternative.source) == sources.end()) {
			sources.push_back(alternative.source);
		}
	}
}

static string SelectedOutputSource(const DPJoinNode &node, const string &local_source) {
	if (!node.fqp_selected_alternative.IsValid()) {
		return local_source;
	}
	auto index = node.fqp_selected_alternative.GetIndex();
	D_ASSERT(index < node.fqp_alternatives.size());
	return node.fqp_alternatives[index].source;
}

static bool LookupMockCost(ClientContext &context, const string &source, const string &sql, ExplainCost &cost) {
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	FQPMockCostKey keys[] = {{source, sql}, {source, ""}, {"", sql}, {"", ""}};
	for (auto &key : keys) {
		auto entry = state.mock_costs.find(key);
		if (entry != state.mock_costs.end()) {
			cost = entry->second;
			return true;
		}
	}
	return false;
}

void FQPOptimizer::Clear(ClientContext &context) {
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.mock_costs.clear();
	state.last_deparse.clear();
}

void FQPOptimizer::SetMockCost(ClientContext &context, const string &source, const string &sql, double startup_cost,
                               double total_cost, int64_t rows, int width) {
	if (!std::isfinite(startup_cost) || !std::isfinite(total_cost) || startup_cost < 0 || total_cost < startup_cost) {
		throw InvalidInputException(
		    "FQP mock costs must be finite, non-negative, and total cost must be >= startup cost");
	}
	if (rows <= 0 || width <= 0) {
		throw InvalidInputException("FQP mock rows and width must be positive");
	}
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.mock_costs[{source, sql}] = {startup_cost, total_cost, idx_t(rows), width};
}

void FQPOptimizer::SetHTTPConfig(ClientContext &context, const string &source_id, const string &host, int base_port,
                                 int timeout_ms) {
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.source_id = source_id;
	state.interface_host = host.empty() ? "127.0.0.1" : host;
	state.interface_base_port = base_port;
	state.timeout_ms = timeout_ms;
	state.http_enabled = true;
}

void FQPOptimizer::SetHTTPEnabled(ClientContext &context, bool enabled) {
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.http_enabled = enabled;
}

void FQPOptimizer::SetDataMovementFactor(ClientContext &context, double factor) {
	if (!std::isfinite(factor) || factor < 0 || factor > 1.0e12) {
		throw InvalidInputException("FQP data movement factor must be finite and between 0 and 1e12");
	}
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.data_movement_factor = factor;
}

void FQPOptimizer::SetRemoteSchema(ClientContext &context, const string &schema) {
	if (schema.empty()) {
		throw InvalidInputException("FQP destination schema must not be empty");
	}
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	state.local_schema = schema;
}

string FQPOptimizer::LastDeparse(ClientContext &context) {
	auto &state = GetFQPState(context);
	std::lock_guard<std::mutex> guard(state.lock);
	return state.last_deparse;
}

bool FQPOptimizer::TryGetBaseCost(QueryGraphManager &query_graph_manager, idx_t relation_id, idx_t fallback_rows,
                                  FQPPlanAlternative &result) {
	auto &context = query_graph_manager.context;
	RelationInfo relation;
	if (!GetRelationInfo(query_graph_manager, relation_id, relation) || !relation.remote) {
		return false;
	}
	double movement_factor;
	string local_schema;
	{
		auto &state = GetFQPState(context);
		std::lock_guard<std::mutex> guard(state.lock);
		movement_factor = state.data_movement_factor;
		local_schema = state.local_schema;
	}

	string sql;
	if (!DeparseBaseCandidate(query_graph_manager, relation_id, relation.source, local_schema, sql)) {
		return false;
	}
	{
		auto &state = GetFQPState(context);
		std::lock_guard<std::mutex> guard(state.lock);
		state.last_deparse = sql;
	}

	fallback_rows = fallback_rows > 0 ? fallback_rows : 600;
	ExplainCost cost {150.0, 150.0 + double(fallback_rows) * 0.05, fallback_rows, 32};
	ExplainCost remote_cost;
	if ((LookupMockCost(context, relation.source, sql, remote_cost) ||
	     RemoteExplainHTTP(GetRemoteConfig(context), relation.source, "base_path", sql, remote_cost)) &&
	    std::isfinite(remote_cost.startup_cost) && std::isfinite(remote_cost.total_cost) &&
	    remote_cost.startup_cost >= 0 && remote_cost.total_cost >= remote_cost.startup_cost) {
		cost = remote_cost;
	}
	result.source = relation.source;
	result.sql = sql;
	result.startup_cost = cost.startup_cost;
	result.rows = cost.rows > 0 ? cost.rows : 1;
	result.width = cost.width > 0 ? cost.width : 32;
	result.movement_cost = movement_factor * double(result.rows) * double(result.width);
	result.total_cost = cost.total_cost + result.movement_cost;
	return true;
}

double FQPOptimizer::EstimateJoinMovementRows(QueryGraphManager &query_graph_manager, const DPJoinNode &left,
	                                            const DPJoinNode &right, const string &destination) {
	string local_source;
	{
		auto &state = GetFQPState(query_graph_manager.context);
		std::lock_guard<std::mutex> guard(state.lock);
		local_source = state.source_id;
	}
	auto target = destination.empty() ? local_source : destination;
	auto movement_rows = left.fqp_realization_movement_rows + right.fqp_realization_movement_rows;
	if (SelectedOutputSource(left, local_source) != target) {
		movement_rows += double(left.cardinality);
	}
	if (SelectedOutputSource(right, local_source) != target) {
		movement_rows += double(right.cardinality);
	}
	return movement_rows;
}

vector<FQPPlanAlternative>
FQPOptimizer::GetJoinAlternatives(QueryGraphManager &query_graph_manager, JoinRelationSet &set, DPJoinNode &left,
                                  DPJoinNode &right, const vector<reference<NeighborInfo>> &possible_connections) {
	vector<FQPPlanAlternative> results;
	auto &context = query_graph_manager.context;
	if (possible_connections.empty()) {
		return results;
	}

	vector<string> sources;
	AddCandidateSources(left, sources);
	AddCandidateSources(right, sources);
	string local_source;
	string local_schema;
	double movement_factor;
	{
		auto &state = GetFQPState(context);
		std::lock_guard<std::mutex> guard(state.lock);
		local_source = state.source_id;
		local_schema = state.local_schema;
		movement_factor = state.data_movement_factor;
	}

	for (auto &source : sources) {
		if (!local_source.empty() && source == local_source) {
			continue;
		}
		string sql;
		if (!DeparseJoinCandidate(query_graph_manager, set, source, local_schema, sql)) {
			continue;
		}
		{
			auto &state = GetFQPState(context);
			std::lock_guard<std::mutex> guard(state.lock);
			state.last_deparse = sql;
		}
		ExplainCost cost;
		if (!LookupMockCost(context, source, sql, cost) &&
		    !RemoteExplainHTTP(GetRemoteConfig(context), source, "join_path", sql, cost)) {
			continue;
		}
		if (!std::isfinite(cost.startup_cost) || !std::isfinite(cost.total_cost) || cost.startup_cost < 0 ||
		    cost.total_cost < cost.startup_cost) {
			continue;
		}
		FQPPlanAlternative candidate;
		candidate.source = source;
		candidate.sql = sql;
		candidate.startup_cost = cost.startup_cost;
		candidate.rows = cost.rows > 0 ? cost.rows : 1;
		candidate.width = cost.width > 0 ? cost.width : 32;
		candidate.movement_cost = movement_factor * double(candidate.rows) * double(candidate.width);
		candidate.realization_movement_rows =
		    EstimateJoinMovementRows(query_graph_manager, left, right, candidate.source);
		candidate.total_cost = cost.total_cost + candidate.movement_cost;
		results.push_back(std::move(candidate));
	}
	return results;
}

class PhysicalFQPExchange : public PhysicalOperator {
public:
	PhysicalFQPExchange(vector<LogicalType> types, FQPPlanAlternative alternative, string scan_kind,
	                    idx_t estimated_cardinality)
	    : PhysicalOperator(PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      alternative(std::move(alternative)), scan_kind(std::move(scan_kind)) {
	}

	string GetName() const override {
		return "FQP_DATA_EXCHANGE";
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["FQP Annotation"] =
		    alternative.source.empty() ? "remote (source=unknown)" : "remote (source=" + alternative.source + ")";
		result["FQP Scan Kind"] = scan_kind;
		result["FQP Data Movement Cost"] = to_string(alternative.movement_cost);
		result["FQP Op Cost"] = to_string(alternative.total_cost - alternative.movement_cost);
		result["FQP Remote SQL"] = alternative.sql;
		SetEstimatedCardinality(result, estimated_cardinality);
		return result;
	}

	OperatorResultType Execute(ExecutionContext &, DataChunk &, DataChunk &, GlobalOperatorState &,
	                           OperatorState &) const override {
		throw InvalidInputException(
		    "Execution reached an FQP data exchange node; the FQP orchestrator did not intercept the plan");
	}

	bool RequiresFinalExecute() const override {
		return true;
	}

	OperatorFinalizeResultType FinalExecute(ExecutionContext &, DataChunk &, GlobalOperatorState &,
	                                        OperatorState &) const override {
		throw InvalidInputException(
		    "Execution reached an FQP data exchange node; the FQP orchestrator did not intercept the plan");
	}

private:
	FQPPlanAlternative alternative;
	string scan_kind;
};

class LogicalFQPExchange : public LogicalExtensionOperator {
public:
	LogicalFQPExchange(unique_ptr<LogicalOperator> child, FQPPlanAlternative alternative, string scan_kind,
	                   idx_t estimated_cardinality)
	    : alternative(std::move(alternative)), scan_kind(std::move(scan_kind)) {
		children.push_back(std::move(child));
		SetEstimatedCardinality(estimated_cardinality);
	}

	string GetName() const override {
		return "FQP_DATA_EXCHANGE";
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["FQP Annotation"] =
		    alternative.source.empty() ? "remote (source=unknown)" : "remote (source=" + alternative.source + ")";
		result["FQP Scan Kind"] = scan_kind;
		result["FQP Data Movement Cost"] = to_string(alternative.movement_cost);
		result["FQP Op Cost"] = to_string(alternative.total_cost - alternative.movement_cost);
		result["FQP Remote SQL"] = alternative.sql;
		SetParamsEstimatedCardinality(result);
		return result;
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}

	idx_t EstimateCardinality(ClientContext &) override {
		return estimated_cardinality;
	}

	bool SupportSerialization() const override {
		return false;
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		auto &result = planner.Make<PhysicalFQPExchange>(child.types, alternative, scan_kind, estimated_cardinality);
		result.children.push_back(child);
		return result;
	}

protected:
	void ResolveTypes() override {
		types = children[0]->types;
	}

private:
	FQPPlanAlternative alternative;
	string scan_kind;
};

unique_ptr<LogicalOperator> FQPOptimizer::WrapPlan(unique_ptr<LogicalOperator> plan, const DPJoinNode &node) {
	if (!node.fqp_selected_alternative.IsValid()) {
		return plan;
	}
	auto index = node.fqp_selected_alternative.GetIndex();
	if (index >= node.fqp_alternatives.size()) {
		throw InternalException("Selected FQP alternative is out of range");
	}
	return make_uniq<LogicalFQPExchange>(std::move(plan), node.fqp_alternatives[index],
	                                     node.is_leaf ? "baserel" : "joinrel", node.cardinality);
}

} // namespace duckdb
