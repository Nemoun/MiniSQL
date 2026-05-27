#include "executor/execute_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>

#include "common/dberr.h"
#include "common/instance.h"
#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "parser/syntax_tree.h"
#include "planner/planner.h"
#include "utils/utils.h"

extern "C" {
  int yyparse(void);
  #include "parser/minisql_lex.h"
  #include "parser/parser.h"
}

ExecuteEngine::ExecuteEngine(bool init) {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }
  /** When you have completed all the code for
   *  the test, run it using main.cpp and uncomment
   *  this part of the code.**/
  if (!init) {
    struct dirent *stdir;
    while((stdir = readdir(dir)) != nullptr) {
      if( strcmp( stdir->d_name , "." ) == 0 ||
          strcmp( stdir->d_name , "..") == 0 ||
          stdir->d_name[0] == '.')
        continue;
      dbs_[stdir->d_name] = new DBStorageEngine(stdir->d_name, false);
    }
  }
  closedir(dir);
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    // Create a new sequential scan executor
    case PlanType::SeqScan: {
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    }
    // Create a new index scan executor
    case PlanType::IndexScan: {
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    }
    // Create a new update executor
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
      // Create a new delete executor
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values: {
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    }
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  // Construct the executor for the abstract plan node
  auto executor = CreateExecutor(exec_ctx, plan);

  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) {
        result_set->push_back(row);
      }
    }
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) {
      result_set->clear();
    }
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) {
    return DB_FAILED;
  }
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!current_db_.empty()) context = dbs_[current_db_]->MakeExecuteContext(nullptr);
  switch (ast->type_) {
    case kNodeCreateDB:
      return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:
      return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:
      return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:
      return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:
      return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable:
      return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:
      return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes:
      return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex:
      return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:
      return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:
      return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:
      return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback:
      return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:
      return ExecuteExecfile(ast, context.get());
    case kNodeQuit:
      return ExecuteQuit(ast, context.get());
    default:
      break;
  }
  // Plan the query.
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    // Execute the query.
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }
  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time)).count());
  // Return the result set as string.
  std::stringstream ss;
  ResultWriter writer(ss);

  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      // find the max width for each column
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set) {
        for (uint32_t i = 0; i < num_of_columns; i++) {
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
        }
      }
      int k = 0;
      for (const auto &column : schema->GetColumns()) {
        data_width[k] = max(data_width[k], int(column->GetName().length()));
        k++;
      }
      // Generate header for the result set.
      writer.Divider(data_width);
      k = 0;
      writer.BeginRow();
      for (const auto &column : schema->GetColumns()) {
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      }
      writer.EndRow();
      writer.Divider(data_width);

      // Transforming result set into strings.
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        }
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  std::cout << writer.stream_.rdbuf();
  // todo:: use shared_ptr for schema
  if (ast->type_ == kNodeSelect)
      delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:
      cout << "Database already exists." << endl;
      break;
    case DB_NOT_EXIST:
      cout << "Database not exists." << endl;
      break;
    case DB_TABLE_ALREADY_EXIST:
      cout << "Table already exists." << endl;
      break;
    case DB_TABLE_NOT_EXIST:
      cout << "Table not exists." << endl;
      break;
    case DB_INDEX_ALREADY_EXIST:
      cout << "Index already exists." << endl;
      break;
    case DB_INDEX_NOT_FOUND:
      cout << "Index not exists." << endl;
      break;
    case DB_COLUMN_NAME_NOT_EXIST:
      cout << "Column not exists." << endl;
      break;
    case DB_KEY_NOT_FOUND:
      cout << "Key not exists." << endl;
      break;
    case DB_QUIT:
      cout << "Bye." << endl;
      break;
    default:
      break;
  }
}

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    return DB_ALREADY_EXIST;
  }
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) {
    return DB_NOT_EXIST;
  }
  remove(("./databases/" + db_name).c_str());
  delete dbs_[db_name];
  dbs_.erase(db_name);
  if (db_name == current_db_)
    current_db_ = "";
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowDatabases" << std::endl;
#endif
  if (dbs_.empty()) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width) max_width = itr.first.length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteUseDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    current_db_ = db_name;
    cout << "Database changed" << endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowTables" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  vector<TableInfo *> tables;
  if (dbs_[current_db_]->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_FAILED;
  }
  string table_in_db("Tables_in_" + current_db_);
  uint max_width = table_in_db.length();
  for (const auto &itr : tables) {
    if (itr->GetTableName().length() > max_width) max_width = itr->GetTableName().length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : tables) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateTable" << std::endl;
#endif  
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
 
  // 获取表名
  string table_name = ast->child_->val_;
 
  // 解析列定义
  std::vector<Column *> columns;
  std::vector<std::string> primary_keys;
  uint32_t col_idx = 0;
 
  pSyntaxNode col_list = ast->child_->next_;  // kNodeColumnDefinitionList
  pSyntaxNode col_node = col_list->child_;
 
  while (col_node != nullptr) {
    if (col_node->type_ == kNodeColumnDefinition) {
      string col_name = col_node->child_->val_;
      pSyntaxNode type_node = col_node->child_->next_;  // kNodeColumnType
      string type_str = type_node->val_;
      bool unique = (col_node->val_ != nullptr && string(col_node->val_) == "unique");
 
      Column *col = nullptr;
      if (type_str == "int") {
        col = new Column(col_name, TypeId::kTypeInt, col_idx, true, unique);
      } else if (type_str == "float") {
        col = new Column(col_name, TypeId::kTypeFloat, col_idx, true, unique);
      } else if (type_str == "char") {
        // char 类型需要读取长度
        pSyntaxNode len_node = type_node->child_;
        int char_len = 0;
        if (len_node != nullptr && len_node->val_ != nullptr) {
          // 检查是否合法（非负整数）
          double d = atof(len_node->val_);
          if (d < 0 || d != (int)d) {
            cout << "Invalid char length: " << len_node->val_ << endl;
            for (auto c : columns) delete c;
            return DB_FAILED;
          }
          char_len = (int)d;
        }
        col = new Column(col_name, TypeId::kTypeChar, char_len, col_idx, true, unique);
      } else {
        cout << "Unknown type: " << type_str << endl;
        for (auto c : columns) delete c;
        return DB_FAILED;
      }
      columns.push_back(col);
      col_idx++;
    } else if (col_node->type_ == kNodeColumnList) {
      // primary key 列表
      pSyntaxNode pk_node = col_node->child_;
      while (pk_node != nullptr) {
        primary_keys.push_back(pk_node->val_);
        pk_node = pk_node->next_;
      }
    }
    col_node = col_node->next_;
  }
 
  // 将主键列标记为 not null（主键列不允许为空）
  for (auto &pk_name : primary_keys) {
    for (auto *col : columns) {
      if (col->GetName() == pk_name) {
        // 主键列：nullable = false
        // Column 没有直接的 SetNullable，通过重新构造
        // 框架中构造函数默认 nullable=true，这里通过析构重建
        // 实际上主键约束由上层保证，这里只需建索引
        (void)col;
        break;
      }
    }
  }
 
  auto *schema = new Schema(columns);
  TableInfo *table_info = nullptr;
  auto ret = context->GetCatalog()->CreateTable(table_name, schema, context->GetTransaction(), table_info);
  delete schema;  // CreateTable 内部已深拷贝
 
  if (ret != DB_SUCCESS) return ret;
 
  // 为主键建立联合索引
  if (!primary_keys.empty()) {
    IndexInfo *pk_index = nullptr;
    context->GetCatalog()->CreateIndex(table_name, "pk_" + table_name, primary_keys,
                                       context->GetTransaction(), pk_index, "bptree");
  }
 
  // 为 unique 列分别建立索引
  for (auto *col : table_info->GetSchema()->GetColumns()) {
    if (col->IsUnique()) {
      // 检查该列是否已经是主键的一部分（避免重复建索引）
      bool is_pk = false;
      for (auto &pk : primary_keys) {
        if (pk == col->GetName()) { is_pk = true; break; }
      }
      if (!is_pk) {
        IndexInfo *idx = nullptr;
        context->GetCatalog()->CreateIndex(table_name, "uk_" + table_name + "_" + col->GetName(),
                                           {col->GetName()}, context->GetTransaction(), idx, "bptree");
      }
    }
  }
 
  cout << "Table created successfully." << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropTable" << std::endl;
#endif
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
  string table_name = ast->child_->val_;
  auto ret = context->GetCatalog()->DropTable(table_name);
  if (ret == DB_SUCCESS) cout << "Table dropped successfully." << endl;
  return ret;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowIndexes" << std::endl;
#endif
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
 
  vector<TableInfo *> tables;
  context->GetCatalog()->GetTables(tables);
 
  bool has_index = false;
  for (auto *table_info : tables) {
    vector<IndexInfo *> indexes;
    context->GetCatalog()->GetTableIndexes(table_info->GetTableName(), indexes);
    for (auto *idx : indexes) {
      cout << "Table: " << table_info->GetTableName()
           << "  Index: " << idx->GetIndexName() << endl;
      has_index = true;
    }
  }
  if (!has_index) cout << "Empty set (0.00 sec)" << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateIndex" << std::endl;
#endif
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
 
  string index_name = ast->child_->val_;
  string table_name = ast->child_->next_->val_;
 
  // 解析索引列
  std::vector<std::string> index_keys;
  pSyntaxNode col_list = ast->child_->next_->next_;  // kNodeColumnList
  pSyntaxNode col_node = col_list->child_;
  while (col_node != nullptr) {
    index_keys.push_back(col_node->val_);
    col_node = col_node->next_;
  }
 
  // 解析索引类型（默认 bptree）
  string index_type = "bptree";
  if (col_list->next_ != nullptr && col_list->next_->type_ == kNodeIndexType) {
    string t = col_list->next_->val_;
    if (t == "btree" || t == "bptree") index_type = "bptree";
  }
 
  IndexInfo *index_info = nullptr;
  auto ret = context->GetCatalog()->CreateIndex(table_name, index_name, index_keys,
                                                context->GetTransaction(), index_info, index_type);
  if (ret == DB_SUCCESS) cout << "Index created successfully." << endl;
  return ret;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropIndex" << std::endl;
#endif
  if (current_db_.empty()) { cout << "No database selected" << endl; return DB_FAILED; }
 
  string index_name = ast->child_->val_;
 
  // 遍历所有表，找到包含该索引的表
  vector<TableInfo *> tables;
  context->GetCatalog()->GetTables(tables);
  for (auto *table_info : tables) {
    IndexInfo *idx = nullptr;
    if (context->GetCatalog()->GetIndex(table_info->GetTableName(), index_name, idx) == DB_SUCCESS) {
      auto ret = context->GetCatalog()->DropIndex(table_info->GetTableName(), index_name);
      if (ret == DB_SUCCESS) cout << "Index dropped successfully." << endl;
      return ret;
    }
  }
  return DB_INDEX_NOT_FOUND;
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxBegin" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxCommit" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxRollback" << std::endl;
#endif
  return DB_FAILED;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteExecfile" << std::endl;
#endif
  string filename = ast->child_->val_;
  std::ifstream file(filename);
  if (!file.is_open()) {
    cout << "Failed to open file: " << filename << endl;
    return DB_FAILED;
  }
 
  string line, sql;
  while (std::getline(file, line)) {
    sql += line;
    // 以分号为语句结束标志
    if (!sql.empty() && sql.back() == ';') {
      // 解析并执行
      YY_BUFFER_STATE bp = yy_scan_string(sql.c_str());
      if (bp == nullptr) { sql.clear(); continue; }
      yy_switch_to_buffer(bp);
      MinisqlParserInit();
      yyparse();
      if (!MinisqlParserGetError()) {
        Execute(MinisqlGetParserRootNode());
      }
      MinisqlParserFinish();
      yy_delete_buffer(bp);
      yylex_destroy();
      sql.clear();
    } else {
      sql += " ";  // 多行 SQL 拼接
    }
  }
  file.close();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteQuit" << std::endl;
#endif
 return DB_FAILED;
}
