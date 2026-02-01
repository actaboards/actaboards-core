/*
 * Copyright (c) 2024 contributors.
 *
 * The MIT License
 */

#include <graphene/postgres_content/postgres_content_plugin.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/impacted.hpp>
#include <graphene/chain/operation_history_object.hpp>
#include <graphene/protocol/content_card.hpp>
#include <graphene/protocol/permission.hpp>

#include <libpq-fe.h>

namespace graphene { namespace postgres_content {

namespace detail
{

class postgres_content_plugin_impl
{
   public:
      explicit postgres_content_plugin_impl(postgres_content_plugin& _plugin)
         : _self(_plugin)
      {}

      ~postgres_content_plugin_impl()
      {
         if (pg_conn) {
            PQfinish(pg_conn);
            pg_conn = nullptr;
         }
      }

      graphene::chain::database& database()
      {
         return _self.database();
      }

      bool connect_to_postgres();
      bool create_tables();
      void on_block(const signed_block& b);
      std::string get_object_id_from_result(const operation_result& result);

      // Operation handlers
      void handle_content_card_create(const content_card_create_operation& op,
                                       uint32_t block_num, fc::time_point_sec block_time,
                                       const std::string& trx_id, const std::string& object_id);
      void handle_content_card_update(const content_card_update_operation& op,
                                       uint32_t block_num, fc::time_point_sec block_time,
                                       const std::string& trx_id, const std::string& object_id);
      void handle_content_card_remove(const content_card_remove_operation& op,
                                       uint32_t block_num, fc::time_point_sec block_time,
                                       const std::string& trx_id);
      void handle_permission_create(const permission_create_operation& op,
                                     uint32_t block_num, fc::time_point_sec block_time,
                                     const std::string& trx_id, const std::string& object_id);
      void handle_permission_create_many(const permission_create_many_operation& op,
                                     uint32_t block_num, fc::time_point_sec block_time,
                                     const std::string& trx_id, const flat_set<object_id_type>& new_objects);
      void handle_permission_remove(const permission_remove_operation& op,
                                     uint32_t block_num, fc::time_point_sec block_time,
                                     const std::string& trx_id);

      postgres_content_plugin& _self;
      PGconn* pg_conn = nullptr;

      std::string _postgres_url;
      uint32_t _start_block = 0;

   private:
      std::string escape_string(const std::string& input);
      bool execute_sql(const std::string& sql);
};

std::string postgres_content_plugin_impl::escape_string(const std::string& input)
{
   if (!pg_conn) return input;

   char* escaped = PQescapeLiteral(pg_conn, input.c_str(), input.length());
   if (!escaped) return "''";

   std::string result(escaped);
   PQfreemem(escaped);
   return result;
}

bool postgres_content_plugin_impl::execute_sql(const std::string& sql)
{
   if (!pg_conn) return false;

   PGresult* res = PQexec(pg_conn, sql.c_str());
   ExecStatusType status = PQresultStatus(res);

   bool success = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);

   if (!success) {
      elog("PostgreSQL error: ${e}", ("e", PQerrorMessage(pg_conn)));
      elog("SQL: ${s}", ("s", sql));
   }

   PQclear(res);
   return success;
}

bool postgres_content_plugin_impl::connect_to_postgres()
{
   pg_conn = PQconnectdb(_postgres_url.c_str());

   if (PQstatus(pg_conn) != CONNECTION_OK) {
      elog("PostgreSQL connection failed: ${e}", ("e", PQerrorMessage(pg_conn)));
      PQfinish(pg_conn);
      pg_conn = nullptr;
      return false;
   }

   ilog("PostgreSQL connection successful");
   return true;
}

bool postgres_content_plugin_impl::create_tables()
{
   const std::string sql = R"(
      CREATE TABLE IF NOT EXISTS indexer_content_cards (
         id SERIAL PRIMARY KEY,
         content_card_id VARCHAR(32) NOT NULL,
         subject_account VARCHAR(32) NOT NULL,
         hash VARCHAR(256),
         url TEXT,
         type VARCHAR(64),
         description TEXT,
         content_key TEXT,
         storage_data TEXT,
         block_num BIGINT NOT NULL,
         block_time TIMESTAMP NOT NULL,
         trx_id VARCHAR(64),
         operation_type SMALLINT NOT NULL,
         is_removed BOOLEAN DEFAULT FALSE,
         created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(content_card_id)
      );

      CREATE INDEX IF NOT EXISTS idx_cc_subject ON indexer_content_cards(subject_account);
      CREATE INDEX IF NOT EXISTS idx_cc_block_time ON indexer_content_cards(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_cc_type ON indexer_content_cards(type);
      CREATE INDEX IF NOT EXISTS idx_cc_is_removed ON indexer_content_cards(is_removed);

      CREATE TABLE IF NOT EXISTS indexer_permissions (
         id SERIAL PRIMARY KEY,
         permission_id VARCHAR(32) NOT NULL,
         subject_account VARCHAR(32) NOT NULL,
         operator_account VARCHAR(32) NOT NULL,
         permission_type VARCHAR(64),
         object_id VARCHAR(32),
         content_key TEXT,
         block_num BIGINT NOT NULL,
         block_time TIMESTAMP NOT NULL,
         trx_id VARCHAR(64),
         operation_type SMALLINT NOT NULL,
         is_removed BOOLEAN DEFAULT FALSE,
         created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(permission_id)
      );

      CREATE INDEX IF NOT EXISTS idx_perm_subject ON indexer_permissions(subject_account);
      CREATE INDEX IF NOT EXISTS idx_perm_operator ON indexer_permissions(operator_account);
      CREATE INDEX IF NOT EXISTS idx_perm_object ON indexer_permissions(object_id);
      CREATE INDEX IF NOT EXISTS idx_perm_block_time ON indexer_permissions(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_perm_is_removed ON indexer_permissions(is_removed);
   )";

   if (!execute_sql(sql)) {
      elog("Failed to create tables");
      return false;
   }

   ilog("PostgreSQL tables created/verified");
   return true;
}

std::string postgres_content_plugin_impl::get_object_id_from_result(const operation_result& result)
{
   // Result can be: void_result(0), object_id_type(1), asset(2), generic_operation_result(3), etc.
   if (result.which() == 1) {
      // Direct object_id_type
      return std::string(result.get<object_id_type>());
   }
   return "";
}

void postgres_content_plugin_impl::on_block(const signed_block& b)
{
   if (!pg_conn) return;

   uint32_t block_num = b.block_num();
   if (block_num < _start_block) return;

   graphene::chain::database& db = database();
   const auto& hist = db.get_applied_operations();

   for (const auto& o_op : hist) {
      if (!o_op.valid()) continue;

      const auto& op = o_op->op;
      const auto& result = o_op->result;
      uint32_t trx_in_block = o_op->trx_in_block;

      std::string trx_id;
      if (trx_in_block < b.transactions.size()) {
         trx_id = b.transactions[trx_in_block].id().str();
      }

      // Get created object ID from result (for create operations)
      std::string created_object_id = get_object_id_from_result(result);

      // Check operation type using which()
      int op_type = op.which();

      // Operation IDs: 41=create_cc, 42=update_cc, 43=remove_cc, 44=create_perm, 45=remove_perm
      if (op_type == 41) {
         handle_content_card_create(op.get<content_card_create_operation>(),
                                     block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type == 42) {
         handle_content_card_update(op.get<content_card_update_operation>(),
                                     block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type == 43) {
         handle_content_card_remove(op.get<content_card_remove_operation>(),
                                     block_num, b.timestamp, trx_id);
      }
      else if (op_type == 44) {
         handle_permission_create(op.get<permission_create_operation>(),
                                   block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type == 45) {
         handle_permission_remove(op.get<permission_remove_operation>(),
                                   block_num, b.timestamp, trx_id);
      }
      else if (op_type == 64) {
         flat_set<object_id_type> new_objects;
         if (result.which() == 3) {
            new_objects = result.get<generic_operation_result>().new_objects;
         }
         handle_permission_create_many(op.get<permission_create_many_operation>(),
                                   block_num, b.timestamp, trx_id, new_objects);
      }
   }
}

void postgres_content_plugin_impl::handle_content_card_create(
   const content_card_create_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));
   std::string content_card_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string sql = "INSERT INTO indexer_content_cards "
      "(content_card_id, subject_account, hash, url, type, description, content_key, storage_data, "
      "block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(content_card_id) + ", "
      + escape_string(subject_account) + ", "
      + escape_string(op.hash) + ", "
      + escape_string(op.url) + ", "
      + escape_string(op.type) + ", "
      + escape_string(op.description) + ", "
      + escape_string(op.content_key) + ", "
      + escape_string(op.storage_data) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "41, FALSE) "
      "ON CONFLICT (content_card_id) DO UPDATE SET "
      "hash = EXCLUDED.hash, url = EXCLUDED.url, type = EXCLUDED.type, "
      "description = EXCLUDED.description, content_key = EXCLUDED.content_key, "
      "storage_data = EXCLUDED.storage_data";

   if (!execute_sql(sql)) {
      elog("Failed to insert content_card_create: block ${b}", ("b", block_num));
   } else {
      ilog("Indexed content_card_create at block ${b}, id ${id}", ("b", block_num)("id", content_card_id));
   }
}

void postgres_content_plugin_impl::handle_content_card_update(
   const content_card_update_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));
   std::string content_card_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string sql = "INSERT INTO indexer_content_cards "
      "(content_card_id, subject_account, hash, url, type, description, content_key, storage_data, "
      "block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(content_card_id) + ", "
      + escape_string(subject_account) + ", "
      + escape_string(op.hash) + ", "
      + escape_string(op.url) + ", "
      + escape_string(op.type) + ", "
      + escape_string(op.description) + ", "
      + escape_string(op.content_key) + ", "
      + escape_string(op.storage_data) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "42, FALSE) "
      "ON CONFLICT (content_card_id) DO UPDATE SET "
      "hash = EXCLUDED.hash, url = EXCLUDED.url, type = EXCLUDED.type, "
      "description = EXCLUDED.description, content_key = EXCLUDED.content_key, "
      "storage_data = EXCLUDED.storage_data, block_num = EXCLUDED.block_num, "
      "block_time = EXCLUDED.block_time, operation_type = 42";

   if (!execute_sql(sql)) {
      elog("Failed to insert content_card_update: block ${b}", ("b", block_num));
   } else {
      ilog("Indexed content_card_update at block ${b}, id ${id}", ("b", block_num)("id", content_card_id));
   }
}

void postgres_content_plugin_impl::handle_content_card_remove(
   const content_card_remove_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id)
{
   std::string content_id = std::string(object_id_type(op.content_id));

   std::string sql = "UPDATE indexer_content_cards SET "
      "is_removed = TRUE, "
      "block_num = " + std::to_string(block_num) + ", "
      "block_time = to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      "operation_type = 43 "
      "WHERE content_card_id = " + escape_string(content_id);

   if (!execute_sql(sql)) {
      elog("Failed to update content_card_remove: block ${b}", ("b", block_num));
   } else {
      ilog("Indexed content_card_remove at block ${b}, id ${id}", ("b", block_num)("id", content_id));
   }
}

void postgres_content_plugin_impl::handle_permission_create(
   const permission_create_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& new_object_id)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));
   std::string operator_account = std::string(object_id_type(op.operator_account));
   std::string ref_object_id = op.object_id.valid() ? std::string(object_id_type(*op.object_id)) : "";
   std::string permission_id = new_object_id.empty() ? ("pending-" + trx_id) : new_object_id;

   std::string sql = "INSERT INTO indexer_permissions "
      "(permission_id, subject_account, operator_account, permission_type, object_id, content_key, "
      "block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(permission_id) + ", "
      + escape_string(subject_account) + ", "
      + escape_string(operator_account) + ", "
      + escape_string(op.permission_type) + ", "
      + escape_string(ref_object_id) + ", "
      + escape_string(op.content_key) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "44, FALSE) "
      "ON CONFLICT (permission_id) DO UPDATE SET "
      "permission_type = EXCLUDED.permission_type, content_key = EXCLUDED.content_key";

   if (!execute_sql(sql)) {
      elog("Failed to insert permission_create: block ${b}", ("b", block_num));
   } else {
      ilog("Indexed permission_create at block ${b}, id ${id}", ("b", block_num)("id", permission_id));
   }
}

void postgres_content_plugin_impl::handle_permission_create_many(
   const permission_create_many_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const flat_set<object_id_type>& new_objects)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));

   auto obj_it = new_objects.begin();
   for (size_t i = 0; i < op.permissions.size(); ++i) {
      const auto& perm = op.permissions[i];
      std::string operator_account = std::string(object_id_type(perm.operator_account));
      std::string ref_object_id = perm.object_id.valid() ? std::string(object_id_type(*perm.object_id)) : "";
      std::string permission_id;
      if (obj_it != new_objects.end()) {
         permission_id = std::string(*obj_it);
         ++obj_it;
      } else {
         permission_id = "pending-" + trx_id + "-" + std::to_string(i);
      }

      std::string sql = "INSERT INTO indexer_permissions "
         "(permission_id, subject_account, operator_account, permission_type, object_id, content_key, "
         "block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
         + escape_string(permission_id) + ", "
         + escape_string(subject_account) + ", "
         + escape_string(operator_account) + ", "
         + escape_string(perm.permission_type) + ", "
         + escape_string(ref_object_id) + ", "
         + escape_string(perm.content_key) + ", "
         + std::to_string(block_num) + ", "
         "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
         + escape_string(trx_id) + ", "
         "64, FALSE) "
         "ON CONFLICT (permission_id) DO UPDATE SET "
         "permission_type = EXCLUDED.permission_type, content_key = EXCLUDED.content_key";

      if (!execute_sql(sql)) {
         elog("Failed to insert permission_create_many: block ${b}", ("b", block_num));
      } else {
         ilog("Indexed permission_create_many at block ${b}, id ${id}", ("b", block_num)("id", permission_id));
      }
   }
}

void postgres_content_plugin_impl::handle_permission_remove(
   const permission_remove_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id)
{
   std::string permission_id = std::string(object_id_type(op.permission_id));

   std::string sql = "UPDATE indexer_permissions SET "
      "is_removed = TRUE, "
      "block_num = " + std::to_string(block_num) + ", "
      "block_time = to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      "operation_type = 45 "
      "WHERE permission_id = " + escape_string(permission_id);

   if (!execute_sql(sql)) {
      elog("Failed to update permission_remove: block ${b}", ("b", block_num));
   } else {
      ilog("Indexed permission_remove at block ${b}, id ${id}", ("b", block_num)("id", permission_id));
   }
}

} // end namespace detail

postgres_content_plugin::postgres_content_plugin(graphene::app::application& app) :
   plugin(app),
   my(std::make_unique<detail::postgres_content_plugin_impl>(*this))
{
}

postgres_content_plugin::~postgres_content_plugin() = default;

std::string postgres_content_plugin::plugin_name() const
{
   return "postgres_content";
}

std::string postgres_content_plugin::plugin_description() const
{
   return "Indexes content_cards and permissions to PostgreSQL database.";
}

void postgres_content_plugin::plugin_set_program_options(
   boost::program_options::options_description& cli,
   boost::program_options::options_description& cfg)
{
   cli.add_options()
      ("postgres-content-url", boost::program_options::value<std::string>(),
         "PostgreSQL connection URL (e.g., postgresql://user:pass@localhost/dbname)")
      ("postgres-content-start-block", boost::program_options::value<uint32_t>()->default_value(0),
         "Start indexing from this block number (default: 0)")
      ;
   cfg.add(cli);
}

void postgres_content_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{
   ilog("postgres_content: plugin_initialize()");

   if (options.count("postgres-content-url") > 0) {
      my->_postgres_url = options["postgres-content-url"].as<std::string>();
   } else {
      wlog("postgres_content: No --postgres-content-url specified, plugin will be disabled");
      return;
   }

   if (options.count("postgres-content-start-block") > 0) {
      my->_start_block = options["postgres-content-start-block"].as<uint32_t>();
   }

   // Connect to database on applied_block signal
   database().applied_block.connect([this](const signed_block& b) {
      my->on_block(b);
   });
}

void postgres_content_plugin::plugin_startup()
{
   ilog("postgres_content: plugin_startup()");

   if (my->_postgres_url.empty()) {
      wlog("postgres_content: Plugin disabled (no URL configured)");
      return;
   }

   if (!my->connect_to_postgres()) {
      FC_THROW_EXCEPTION(fc::exception, "Failed to connect to PostgreSQL at ${url}",
                         ("url", my->_postgres_url));
   }

   if (!my->create_tables()) {
      FC_THROW_EXCEPTION(fc::exception, "Failed to create PostgreSQL tables");
   }

   ilog("postgres_content: Plugin started successfully");
}

void postgres_content_plugin::plugin_shutdown()
{
   ilog("postgres_content: plugin_shutdown()");
}

} } // graphene::postgres_content
