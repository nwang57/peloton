//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// schema_catalog.cpp
//
// Identification: src/catalog/schema_catalog.cpp
//
// Copyright (c) 2015-17, Carnegie Mellon University Index Group
//
//===----------------------------------------------------------------------===//

#include "catalog/schema_catalog.h"

#include "catalog/catalog.h"
#include "catalog/system_catalogs.h"
#include "concurrency/transaction_context.h"
#include "executor/logical_tile.h"
#include "storage/data_table.h"
#include "storage/database.h"
#include "storage/tuple.h"
#include "type/value_factory.h"
#include "codegen/buffering_consumer.h"
#include "expression/expression_util.h"

namespace peloton {
namespace catalog {

SchemaCatalogObject::SchemaCatalogObject(codegen::WrappedTuple wrapped_tuple,
                                         concurrency::TransactionContext *txn)
    : schema_oid(wrapped_tuple.GetValue(SchemaCatalog::ColumnId::SCHEMA_OID)
                     .GetAs<oid_t>()),
      schema_name(
          wrapped_tuple.GetValue(SchemaCatalog::ColumnId::SCHEMA_NAME).ToString()),
      txn(txn) {}

SchemaCatalog::SchemaCatalog(
    storage::Database *database, UNUSED_ATTRIBUTE type::AbstractPool *pool,
    UNUSED_ATTRIBUTE concurrency::TransactionContext *txn)
    : AbstractCatalog(SCHEMA_CATALOG_OID, SCHEMA_CATALOG_NAME,
                      InitializeSchema().release(), database) {
  // Add indexes for pg_namespace
  AddIndex({0}, SCHEMA_CATALOG_PKEY_OID, SCHEMA_CATALOG_NAME "_pkey",
           IndexConstraintType::PRIMARY_KEY);
  AddIndex({1}, SCHEMA_CATALOG_SKEY0_OID, SCHEMA_CATALOG_NAME "_skey0",
           IndexConstraintType::UNIQUE);
}

SchemaCatalog::~SchemaCatalog() {}

/*@brief   private function for initialize schema of pg_namespace
 * @return  unqiue pointer to schema
 */
std::unique_ptr<catalog::Schema> SchemaCatalog::InitializeSchema() {
  const std::string not_null_constraint_name = "not_null";
  const std::string primary_key_constraint_name = "primary_key";

  auto schema_id_column = catalog::Column(
      type::TypeId::INTEGER, type::Type::GetTypeSize(type::TypeId::INTEGER),
      "schema_oid", true);
  schema_id_column.AddConstraint(catalog::Constraint(
      ConstraintType::PRIMARY, primary_key_constraint_name));
  schema_id_column.AddConstraint(
      catalog::Constraint(ConstraintType::NOTNULL, not_null_constraint_name));

  auto schema_name_column = catalog::Column(
      type::TypeId::VARCHAR, max_name_size, "schema_name", false);
  schema_name_column.AddConstraint(
      catalog::Constraint(ConstraintType::NOTNULL, not_null_constraint_name));

  std::unique_ptr<catalog::Schema> schema(
      new catalog::Schema({schema_id_column, schema_name_column}));
  return schema;
}

bool SchemaCatalog::InsertSchema(oid_t schema_oid,
                                 const std::string &schema_name,
                                 type::AbstractPool *pool,
                                 concurrency::TransactionContext *txn) {
  // Create the tuple first
  std::vector<std::vector<ExpressionPtr>> tuples;

  auto val0 = type::ValueFactory::GetIntegerValue(schema_oid);
  auto val1 = type::ValueFactory::GetVarcharValue(schema_name, nullptr);

  tuples.push_back(std::vector<ExpressionPtr>());
  auto &values = tuples[0];
  values.push_back(ExpressionPtr(new expression::ConstantValueExpression(val0)));
  values.push_back(ExpressionPtr(new expression::ConstantValueExpression(val1)));

  // Insert the tuple
  return InsertTupleWithCompiledPlan(&tuples, txn);
}

bool SchemaCatalog::DeleteSchema(const std::string &schema_name,
                                 concurrency::TransactionContext *txn) {
  std::vector<oid_t> column_ids(all_column_ids);

  auto *schema_name_expr =
      new expression::TupleValueExpression(type::TypeId::INTEGER, 0,
                                           ColumnId::SCHEMA_NAME);
  schema_name_expr->SetBoundOid(catalog_table_->GetDatabaseOid(), catalog_table_->GetOid(), ColumnId::SCHEMA_NAME);


  expression::AbstractExpression *schema_name_const_expr =
      expression::ExpressionUtil::ConstantValueFactory(
          type::ValueFactory::GetVarcharValue(schema_name, nullptr).Copy());
  expression::AbstractExpression *schema_name_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, schema_name_expr, schema_name_const_expr);

  return DeleteWithCompiledSeqScan(column_ids, schema_name_equality_expr, txn);
}

std::shared_ptr<SchemaCatalogObject> SchemaCatalog::GetSchemaObject(
    const std::string &schema_name, concurrency::TransactionContext *txn) {
  if (txn == nullptr) {
    throw CatalogException("Transaction is invalid!");
  }
  // get from pg_namespace, index scan
  std::vector<oid_t> column_ids(all_column_ids);

  auto *schema_name_expr =
      new expression::TupleValueExpression(type::TypeId::INTEGER, 0,
                                           ColumnId::SCHEMA_NAME);
  schema_name_expr->SetBoundOid(catalog_table_->GetDatabaseOid(), catalog_table_->GetOid(), ColumnId::SCHEMA_NAME);


  expression::AbstractExpression *schema_name_const_expr =
      expression::ExpressionUtil::ConstantValueFactory(
          type::ValueFactory::GetVarcharValue(schema_name, nullptr).Copy());
  expression::AbstractExpression *schema_name_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, schema_name_expr, schema_name_const_expr);


  auto result_tuples = GetResultWithCompiledSeqScan(column_ids, schema_name_equality_expr, txn);

  if (result_tuples.size() == 1) {
    auto schema_object =
        std::make_shared<SchemaCatalogObject>(result_tuples[0], txn);
    // TODO: we don't have cache for schema object right now
    return schema_object;
  }

  // return empty object if not found
  return nullptr;
}

}  // namespace catalog
}  // namespace peloton
