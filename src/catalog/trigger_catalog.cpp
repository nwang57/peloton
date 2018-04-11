//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// trigger_catalog.cpp
//
// Identification: src/catalog/trigger_catalog.cpp
//
// Copyright (c) 2015-2017, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "expression/expression_util.h"
#include "codegen/buffering_consumer.h"
#include "catalog/trigger_catalog.h"

#include "catalog/catalog.h"
#include "catalog/database_catalog.h"
#include "catalog/table_catalog.h"
#include "storage/data_table.h"
#include "type/value_factory.h"

namespace peloton {
namespace catalog {

TriggerCatalog &TriggerCatalog::GetInstance(concurrency::TransactionContext *txn) {
  static TriggerCatalog trigger_catalog{txn};
  return trigger_catalog;
}

TriggerCatalog::TriggerCatalog(concurrency::TransactionContext *txn)
    : AbstractCatalog("CREATE TABLE " CATALOG_DATABASE_NAME
                      "." TRIGGER_CATALOG_NAME
                      " ("
                      "oid          INT NOT NULL PRIMARY KEY, "
                      "tgrelid      INT NOT NULL, "
                      "tgname       VARCHAR NOT NULL, "
                      "tgfoid       VARCHAR, "
                      "tgtype       INT NOT NULL, "
                      "tgargs       VARCHAR, "
                      "tgqual       VARBINARY, "
                      "timestamp    TIMESTAMP NOT NULL);",
                      txn) {
  // Add secondary index here if necessary
  Catalog::GetInstance()->CreateIndex(
      CATALOG_DATABASE_NAME, TRIGGER_CATALOG_NAME,
      {ColumnId::TABLE_OID, ColumnId::TRIGGER_TYPE},
      TRIGGER_CATALOG_NAME "_skey0", false, IndexType::BWTREE, txn);

  Catalog::GetInstance()->CreateIndex(
      CATALOG_DATABASE_NAME, TRIGGER_CATALOG_NAME, {ColumnId::TABLE_OID},
      TRIGGER_CATALOG_NAME "_skey1", false, IndexType::BWTREE, txn);

  Catalog::GetInstance()->CreateIndex(
      CATALOG_DATABASE_NAME, TRIGGER_CATALOG_NAME,
      {ColumnId::TRIGGER_NAME, ColumnId::TABLE_OID},
      TRIGGER_CATALOG_NAME "_skey2", false, IndexType::BWTREE, txn);
}

TriggerCatalog::~TriggerCatalog() {}

bool TriggerCatalog::InsertTrigger(oid_t table_oid, std::string trigger_name,
                                   int16_t trigger_type, std::string proc_oid,
                                   std::string function_arguments,
                                   type::Value fire_condition,
                                   type::Value timestamp,
                                   type::AbstractPool *pool,
                                   concurrency::TransactionContext *txn) {
  std::unique_ptr<storage::Tuple> tuple(
      new storage::Tuple(catalog_table_->GetSchema(), true));

  LOG_INFO("type of trigger inserted:%d", trigger_type);

  auto val0 = type::ValueFactory::GetIntegerValue(GetNextOid());
  auto val1 = type::ValueFactory::GetIntegerValue(table_oid);
  auto val2 = type::ValueFactory::GetVarcharValue(trigger_name);
  auto val3 = type::ValueFactory::GetVarcharValue(proc_oid);
  auto val4 = type::ValueFactory::GetIntegerValue(trigger_type);
  auto val5 = type::ValueFactory::GetVarcharValue(function_arguments);
  auto val6 = fire_condition;
  auto val7 = timestamp;

  tuple->SetValue(ColumnId::TRIGGER_OID, val0, pool);
  tuple->SetValue(ColumnId::TABLE_OID, val1, pool);
  tuple->SetValue(ColumnId::TRIGGER_NAME, val2, pool);
  tuple->SetValue(ColumnId::FUNCTION_OID, val3, pool);
  tuple->SetValue(ColumnId::TRIGGER_TYPE, val4, pool);
  tuple->SetValue(ColumnId::FUNCTION_ARGS, val5, pool);
  tuple->SetValue(ColumnId::FIRE_CONDITION, val6, pool);
  tuple->SetValue(ColumnId::TIMESTAMP, val7, pool);

  // Insert the tuple
  return InsertTuple(std::move(tuple), txn);
}

ResultType TriggerCatalog::DropTrigger(const std::string &database_name,
                                       const std::string &table_name,
                                       const std::string &trigger_name,
                                       concurrency::TransactionContext *txn) {
  if (txn == nullptr) {
    LOG_TRACE("Do not have transaction to drop trigger: %s",
              table_name.c_str());
    return ResultType::FAILURE;
  }

  // Checking if statement is valid
  auto table_object =
      Catalog::GetInstance()->GetTableObject(database_name, table_name, txn);

  oid_t trigger_oid = TriggerCatalog::GetInstance().GetTriggerOid(
      trigger_name, table_object->GetTableOid(), txn);
  if (trigger_oid == INVALID_OID) {
    LOG_TRACE("Cannot find trigger %s to drop!", trigger_name.c_str());
    return ResultType::FAILURE;
  }

  LOG_INFO("trigger %d will be deleted!", trigger_oid);

  bool delete_success =
      DeleteTriggerByName(trigger_name, table_object->GetTableOid(), txn);
  if (delete_success) {
    LOG_DEBUG("Delete trigger successfully");
    // ask target table to update its trigger list variable
    storage::DataTable *target_table =
        catalog::Catalog::GetInstance()->GetTableWithName(database_name,
                                                          table_name, txn);
    target_table->UpdateTriggerListFromCatalog(txn);
    return ResultType::SUCCESS;
  }
  LOG_DEBUG("Failed to delete trigger");
  return ResultType::FAILURE;
}

oid_t TriggerCatalog::GetTriggerOid(std::string trigger_name, oid_t table_oid,
                                    concurrency::TransactionContext *txn) {

  std::vector<oid_t> column_ids({ColumnId::TRIGGER_OID});

  expression::AbstractExpression *name_expr = expression::ExpressionUtil::TupleValueFactory(
      type::TypeId::VARCHAR, 0, ColumnId::TRIGGER_NAME);
  expression::AbstractExpression *name_const_expr = expression::ExpressionUtil::ConstantValueFactory(
      type::ValueFactory::GetVarcharValue(trigger_name, nullptr).Copy());
  expression::AbstractExpression *name_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, name_expr,
          name_const_expr);

  expression::AbstractExpression *oid_expr = expression::ExpressionUtil::TupleValueFactory(
      type::TypeId::INTEGER, 0, ColumnId::TABLE_OID);
  expression::AbstractExpression *oid_const_expr = expression::ExpressionUtil::ConstantValueFactory(
      type::ValueFactory::GetIntegerValue(table_oid).Copy());
  expression::AbstractExpression *oid_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, oid_expr,
          oid_const_expr);

  expression::AbstractExpression *predicate = expression::ExpressionUtil::ConjunctionFactory(
      ExpressionType::CONJUNCTION_AND, name_equality_expr, oid_equality_expr);

  std::vector<codegen::WrappedTuple> result_tuples =
      GetResultWithCompiledSeqScan(column_ids, predicate, txn);

  oid_t trigger_oid = INVALID_OID;
  if (result_tuples.size() == 0) {
    LOG_INFO("trigger %s doesn't exist", trigger_name.c_str());
  } else {
    LOG_INFO("size of the result tiles = %lu", result_tuples.size());
    PL_ASSERT(result_tuples.size() <= 1);
    if (result_tuples.size() != 0) {
      trigger_oid = result_tuples[0].GetValue(0).GetAs<oid_t>();
    }
  }

  return trigger_oid;
}

bool TriggerCatalog::DeleteTriggerByName(const std::string &trigger_name,
                                         oid_t table_oid,
                                         concurrency::TransactionContext *txn) {
  oid_t index_offset = IndexId::NAME_TABLE_KEY_2;
  std::vector<type::Value> values;
  values.push_back(type::ValueFactory::GetVarcharValue(trigger_name).Copy());
  values.push_back(type::ValueFactory::GetIntegerValue(table_oid).Copy());

  return DeleteWithIndexScan(index_offset, values, txn);
}

std::unique_ptr<trigger::TriggerList> TriggerCatalog::GetTriggersByType(
    oid_t table_oid, int16_t trigger_type, concurrency::TransactionContext *txn) {
  LOG_INFO("Get triggers for table %d", table_oid);
  std::vector<oid_t> column_ids(
      {ColumnId::TRIGGER_NAME, ColumnId::FIRE_CONDITION, ColumnId::FUNCTION_OID,
       ColumnId::FUNCTION_ARGS});

  expression::AbstractExpression *type_expr = expression::ExpressionUtil::TupleValueFactory(
      type::TypeId::SMALLINT, 0, ColumnId::TRIGGER_TYPE);
  expression::AbstractExpression *type_const_expr = expression::ExpressionUtil::ConstantValueFactory(
      type::ValueFactory::GetSmallIntValue(trigger_type).Copy());
  expression::AbstractExpression *type_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, type_expr,
          type_const_expr);

  expression::AbstractExpression *oid_expr = expression::ExpressionUtil::TupleValueFactory(
      type::TypeId::INTEGER, 0, ColumnId::TABLE_OID);
  expression::AbstractExpression *oid_const_expr = expression::ExpressionUtil::ConstantValueFactory(
      type::ValueFactory::GetIntegerValue(table_oid).Copy());
  expression::AbstractExpression *oid_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, oid_expr,
          oid_const_expr);

  expression::AbstractExpression *predicate = expression::ExpressionUtil::ConjunctionFactory(
      ExpressionType::CONJUNCTION_AND, type_equality_expr, oid_equality_expr);

  std::vector<codegen::WrappedTuple> result_tuples =
      GetResultWithCompiledSeqScan(column_ids, predicate, txn);

  // carefull! the result could be null!
  LOG_INFO("size of the result tiles = %lu", result_tuples.size());


  // create the trigger list
  std::unique_ptr<trigger::TriggerList> new_trigger_list{
      new trigger::TriggerList()};

  for (unsigned int i = 0; i < result_tuples.size(); i++) {
        // create a new trigger instance
        trigger::Trigger new_trigger(
            result_tuples[i].GetValue(0).ToString(), trigger_type,
            result_tuples[i].GetValue(2).ToString(),
            result_tuples[i].GetValue(3).ToString(),
            result_tuples[i].GetValue(1).GetData());
        new_trigger_list->AddTrigger(new_trigger);

  }

  return new_trigger_list;
}

std::unique_ptr<trigger::TriggerList> TriggerCatalog::GetTriggers(
    oid_t table_oid, concurrency::TransactionContext *txn) {
  LOG_DEBUG("Get triggers for table %d", table_oid);

  std::vector<oid_t> column_ids(
      {ColumnId::TRIGGER_NAME, ColumnId::TRIGGER_TYPE, ColumnId::FIRE_CONDITION,
       ColumnId::FUNCTION_OID, ColumnId::FUNCTION_ARGS});
  expression::AbstractExpression *oid_expr = expression::ExpressionUtil::TupleValueFactory(
      type::TypeId::INTEGER, 0, ColumnId::TABLE_OID);
  expression::AbstractExpression *oid_const_expr = expression::ExpressionUtil::ConstantValueFactory(
      type::ValueFactory::GetIntegerValue(table_oid).Copy());
  expression::AbstractExpression *oid_equality_expr =
      expression::ExpressionUtil::ComparisonFactory(
          ExpressionType::COMPARE_EQUAL, oid_expr,
          oid_const_expr);

  std::vector<codegen::WrappedTuple> result_tuples =
      GetResultWithCompiledSeqScan(column_ids, oid_equality_expr, txn);

  // carefull! the result tile could be null!
  LOG_INFO("size of the result tiles = %lu", result_tuples.size());

// create the trigger list
  std::unique_ptr<trigger::TriggerList> new_trigger_list{
      new trigger::TriggerList()};

  for (unsigned int i = 0; i < result_tuples.size(); i++) {
    // create a new trigger instance
    trigger::Trigger new_trigger(
        result_tuples[i].GetValue(0).ToString(),
        result_tuples[i].GetValue(1).GetAs<int16_t>(),
        result_tuples[i].GetValue(3).ToString(),
        result_tuples[i].GetValue(4).ToString(),
        result_tuples[i].GetValue(2).GetData());
    new_trigger_list->AddTrigger(new_trigger);

  }

  return new_trigger_list;
}

}  // namespace catalog
}  // namespace peloton
