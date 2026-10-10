#include "duckdb/optimizer/join_order/fqp_optimizer_internal.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>

namespace duckdb {
namespace fqp_internal {

static optional_ptr<const LogicalGet> FindLogicalGet(const LogicalOperator &op) {
	auto current = &op;
	while (current) {
		if (current->type == LogicalOperatorType::LOGICAL_GET) {
			return &current->Cast<LogicalGet>();
		}
		if (current->children.size() != 1) {
			return nullptr;
		}
		switch (current->type) {
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_PROJECTION:
			current = current->children[0].get();
			break;
		default:
			return nullptr;
		}
	}
	return nullptr;
}

bool GetRelationInfo(QueryGraphManager &query_graph_manager, idx_t relation_id, RelationInfo &result) {
	auto &relation = query_graph_manager.relation_manager.GetRelation(relation_id);
	auto get = FindLogicalGet(relation.op);
	if (!get) {
		return false;
	}
	auto table = get->GetTable();
	if (table) {
		result.table_name = table->name;
		result.schema_name = table->schema.name;
	} else {
		return false;
	}
	result.remote = StringUtil::CIEquals(result.schema_name, "remote");
	(void)ParseSourcePrefix(result.table_name, result.source);
	return true;
}

static bool DeparseColumnRef(QueryGraphManager &query_graph_manager, const BoundColumnRefExpression &expr,
                             string &result) {
	if (expr.depth != 0) {
		return false;
	}
	auto entry = query_graph_manager.relation_manager.relation_mapping.find(expr.binding.table_index);
	if (entry == query_graph_manager.relation_manager.relation_mapping.end()) {
		return false;
	}
	auto relation_id = entry->second;
	auto &relation = query_graph_manager.relation_manager.GetRelation(relation_id);
	auto get = FindLogicalGet(relation.op);
	if (!get) {
		return false;
	}
	auto column_index = expr.binding.column_index;
	if (!get->projection_ids.empty()) {
		if (column_index >= get->projection_ids.size()) {
			return false;
		}
		column_index = get->projection_ids[column_index];
	}
	auto &column_ids = get->GetColumnIds();
	if (column_index >= column_ids.size()) {
		return false;
	}
	result = "r" + to_string(relation_id) + "." + QuoteIdentifier(get->GetColumnName(column_ids[column_index]));
	return true;
}

static bool DeparseExpression(QueryGraphManager &query_graph_manager, const Expression &expr, string &result);

static bool DeparseComparison(QueryGraphManager &query_graph_manager, const BoundComparisonExpression &expr,
                              string &result) {
	string left;
	string right;
	if (!DeparseExpression(query_graph_manager, *expr.left, left) ||
	    !DeparseExpression(query_graph_manager, *expr.right, right)) {
		return false;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		result = "(" + left + " " + ExpressionTypeToOperator(expr.GetExpressionType()) + " " + right + ")";
		return true;
	default:
		return false;
	}
}

static bool DeparseConjunction(QueryGraphManager &query_graph_manager, const BoundConjunctionExpression &expr,
                               string &result) {
	if (expr.children.empty()) {
		return false;
	}
	string op;
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		op = " AND ";
	} else if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		op = " OR ";
	} else {
		return false;
	}
	vector<string> children;
	for (auto &child : expr.children) {
		string child_sql;
		if (!DeparseExpression(query_graph_manager, *child, child_sql)) {
			return false;
		}
		children.push_back(child_sql);
	}
	result = "(" + StringUtil::Join(children, op) + ")";
	return true;
}

static bool DeparseOptimizedLike(QueryGraphManager &query_graph_manager, const BoundFunctionExpression &expr,
                                 string &result) {
	if (expr.children.size() != 2 || expr.children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto function_name = StringUtil::Lower(expr.function.name);
	if (function_name != "contains" && function_name != "prefix" && function_name != "suffix") {
		return false;
	}
	auto &constant = expr.children[1]->Cast<BoundConstantExpression>().value;
	if (constant.IsNull() || constant.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	string value = StringValue::Get(constant);
	if (function_name == "contains" || function_name == "suffix") {
		value = "%" + value;
	}
	if (function_name == "contains" || function_name == "prefix") {
		value += "%";
	}
	string input;
	if (!DeparseExpression(query_graph_manager, *expr.children[0], input)) {
		return false;
	}
	result = "(" + input + " LIKE " + Value(value).ToSQLString() + ")";
	return true;
}

static bool DeparseExpression(QueryGraphManager &query_graph_manager, const Expression &expr, string &result) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return DeparseColumnRef(query_graph_manager, expr.Cast<BoundColumnRefExpression>(), result);
	case ExpressionClass::BOUND_REF:
		result = expr.Cast<BoundReferenceExpression>().ToString();
		return true;
	case ExpressionClass::BOUND_CONSTANT:
		result = expr.Cast<BoundConstantExpression>().value.ToSQLString();
		return true;
	case ExpressionClass::BOUND_FUNCTION:
		return DeparseOptimizedLike(query_graph_manager, expr.Cast<BoundFunctionExpression>(), result);
	case ExpressionClass::BOUND_COMPARISON:
		return DeparseComparison(query_graph_manager, expr.Cast<BoundComparisonExpression>(), result);
	case ExpressionClass::BOUND_CONJUNCTION:
		return DeparseConjunction(query_graph_manager, expr.Cast<BoundConjunctionExpression>(), result);
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		string child;
		if (!DeparseExpression(query_graph_manager, *cast.child, child)) {
			return false;
		}
		result = "CAST(" + child + " AS " + cast.return_type.ToString() + ")";
		return true;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.children.size() != 1) {
			return false;
		}
		string child;
		if (!DeparseExpression(query_graph_manager, *op.children[0], child)) {
			return false;
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_NOT:
			result = "(NOT " + child + ")";
			return true;
		case ExpressionType::OPERATOR_IS_NULL:
			result = "(" + child + " IS NULL)";
			return true;
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			result = "(" + child + " IS NOT NULL)";
			return true;
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

static bool DeparseRelationRef(QueryGraphManager &query_graph_manager, idx_t relation_id, const string &dest_source,
                               const string &local_schema, string &result) {
	RelationInfo relation;
	if (!GetRelationInfo(query_graph_manager, relation_id, relation)) {
		return false;
	}
	auto schema = relation.schema_name;
	if (!relation.source.empty()) {
		schema = relation.source == dest_source ? local_schema : "remote";
	}
	result = QuoteIdentifier(schema) + "." + QuoteIdentifier(relation.table_name) + " r" + to_string(relation_id);
	return true;
}

static void AppendRequiredTableFilter(QueryGraphManager &query_graph_manager, const TableFilter &filter,
                                      const string &column, vector<string> &predicates) {
	if (filter.filter_type == TableFilterType::OPTIONAL_FILTER) {
		return;
	}
	if (filter.filter_type == TableFilterType::CONJUNCTION_AND) {
		for (auto &child : filter.Cast<ConjunctionAndFilter>().child_filters) {
			AppendRequiredTableFilter(query_graph_manager, *child, column, predicates);
		}
		return;
	}
	string predicate;
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		BoundReferenceExpression column_ref(column, LogicalType::INVALID, 0);
		auto expression = filter.Cast<ExpressionFilter>().ToExpression(column_ref);
		if (!DeparseExpression(query_graph_manager, *expression, predicate)) {
			predicate = filter.ToString(column);
		}
	} else {
		predicate = filter.ToString(column);
	}
	if (std::find(predicates.begin(), predicates.end(), predicate) == predicates.end()) {
		predicates.push_back(std::move(predicate));
	}
}

static bool AppendTableFilters(QueryGraphManager &query_graph_manager, idx_t relation_id, vector<string> &predicates) {
	auto &relation = query_graph_manager.relation_manager.GetRelation(relation_id);
	auto get = FindLogicalGet(relation.op);
	if (!get) {
		return false;
	}
	for (auto &entry : get->table_filters.filters) {
		if (entry.first >= get->names.size()) {
			return false;
		}
		auto column = "r" + to_string(relation_id) + "." + QuoteIdentifier(get->names[entry.first]);
		AppendRequiredTableFilter(query_graph_manager, *entry.second, column, predicates);
	}
	return true;
}

static bool AppendReferencedColumns(QueryGraphManager &query_graph_manager, idx_t relation_id,
                                    vector<string> &projections) {
	auto &relation = query_graph_manager.relation_manager.GetRelation(relation_id);
	auto get = FindLogicalGet(relation.op);
	if (!get) {
		return false;
	}
	for (auto &column_id : get->GetColumnIds()) {
		auto projection =
		    "r" + to_string(relation_id) + "." + QuoteIdentifier(get->GetColumnName(column_id));
		if (std::find(projections.begin(), projections.end(), projection) == projections.end()) {
			projections.push_back(std::move(projection));
		}
	}
	return true;
}

static bool AppendFilterBindings(QueryGraphManager &query_graph_manager, JoinRelationSet &set,
                                 vector<string> &predicates) {
	for (auto &filter_ref : query_graph_manager.GetFilterBindings()) {
		auto &filter = *filter_ref;
		if (!JoinRelationSet::IsSubset(set, filter.set.get())) {
			continue;
		}
		if (filter.join_type != JoinType::INNER && filter.join_type != JoinType::INVALID) {
			return false;
		}
		// contrib/mock_table only propagates join trees whose join clauses are
		// simple equality comparisons. Single-relation restrictions can still
		// use the broader expression deparser below.
		if (filter.set.get().count > 1 &&
		    (filter.filter->GetExpressionClass() != ExpressionClass::BOUND_COMPARISON ||
		     filter.filter->GetExpressionType() != ExpressionType::COMPARE_EQUAL)) {
			return false;
		}
		string predicate;
		if (!DeparseExpression(query_graph_manager, *filter.filter, predicate)) {
			return false;
		}
		if (std::find(predicates.begin(), predicates.end(), predicate) == predicates.end()) {
			predicates.push_back(std::move(predicate));
		}
	}
	return true;
}

bool DeparseJoinCandidate(QueryGraphManager &query_graph_manager, JoinRelationSet &set, const string &dest_source,
                          const string &local_schema, string &sql) {
	vector<string> from_items;
	vector<string> predicates;
	vector<string> projections;

	for (idx_t i = 0; i < set.count; i++) {
		auto relation_id = set.relations[i];
		string rel_sql;
		if (!DeparseRelationRef(query_graph_manager, relation_id, dest_source, local_schema, rel_sql)) {
			return false;
		}
		from_items.push_back(rel_sql);
		if (!AppendTableFilters(query_graph_manager, relation_id, predicates) ||
		    !AppendReferencedColumns(query_graph_manager, relation_id, projections)) {
			return false;
		}
	}

	if (!AppendFilterBindings(query_graph_manager, set, predicates)) {
		return false;
	}

	sql = "SELECT " + (projections.empty() ? "*" : StringUtil::Join(projections, ", ")) + " FROM " +
	      StringUtil::Join(from_items, ", ");
	if (!predicates.empty()) {
		sql += " WHERE " + StringUtil::Join(predicates, " AND ");
	}
	return true;
}

bool DeparseBaseCandidate(QueryGraphManager &query_graph_manager, idx_t relation_id, const string &dest_source,
                          const string &local_schema, string &sql) {
	string relation;
	if (!DeparseRelationRef(query_graph_manager, relation_id, dest_source, local_schema, relation)) {
		return false;
	}
	vector<string> projections;
	if (!AppendReferencedColumns(query_graph_manager, relation_id, projections)) {
		return false;
	}
	sql = "SELECT " +
	      (projections.empty() ? "r" + to_string(relation_id) + ".*" : StringUtil::Join(projections, ", ")) +
	      " FROM " + relation;
	vector<string> predicates;
	if (!AppendTableFilters(query_graph_manager, relation_id, predicates)) {
		return false;
	}
	auto &relation_set = query_graph_manager.set_manager.GetJoinRelation(relation_id);
	if (!AppendFilterBindings(query_graph_manager, relation_set, predicates)) {
		return false;
	}
	if (!predicates.empty()) {
		sql += " WHERE " + StringUtil::Join(predicates, " AND ");
	}
	return true;
}

} // namespace fqp_internal
} // namespace duckdb
