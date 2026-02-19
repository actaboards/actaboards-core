/*
 * Copyright (c) 2024 contributors.
 *
 * The MIT License
 */

#include <graphene/postgres_indexer/postgres_indexer_plugin.hpp>
#include <graphene/chain/impacted.hpp>
#include <graphene/chain/account_evaluator.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/operation_history_object.hpp>
#include <graphene/chain/proposal_object.hpp>
#include <graphene/chain/balance_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/protocol/content_card.hpp>
#include <graphene/protocol/permission.hpp>
#include <graphene/protocol/room.hpp>
#include <graphene/chain/room_object.hpp>

#include <fc/io/json.hpp>
#include <libpq-fe.h>

namespace graphene { namespace postgres_indexer {

namespace detail
{

// Visitor struct for extracting fee/transfer/fill data from operations.
// Must be at namespace scope (not local) so template operator() works in C++11.
struct pg_operation_visitor {
   typedef void result_type;

   share_type fee_amount;
   asset_id_type fee_asset;

   asset_id_type transfer_asset_id;
   share_type transfer_amount;
   account_id_type transfer_from;
   account_id_type transfer_to;

   object_id_type fill_order_id;
   account_id_type fill_account_id;
   asset_id_type fill_pays_asset_id;
   share_type fill_pays_amount;
   asset_id_type fill_receives_asset_id;
   share_type fill_receives_amount;
   double fill_fill_price = 0;
   bool fill_is_maker = false;

   void operator()(const graphene::chain::transfer_operation& o) {
      fee_asset = o.fee.asset_id;
      fee_amount = o.fee.amount;
      transfer_asset_id = o.amount.asset_id;
      transfer_amount = o.amount.amount;
      transfer_from = o.from;
      transfer_to = o.to;
   }

   void operator()(const graphene::chain::fill_order_operation& o) {
      fee_asset = o.fee.asset_id;
      fee_amount = o.fee.amount;
      fill_order_id = o.order_id;
      fill_account_id = o.account_id;
      fill_pays_asset_id = o.pays.asset_id;
      fill_pays_amount = o.pays.amount;
      fill_receives_asset_id = o.receives.asset_id;
      fill_receives_amount = o.receives.amount;
      fill_fill_price = o.fill_price.to_real();
      fill_is_maker = o.is_maker;
   }

   template<typename T>
   void operator()(const T& o) {
      fee_asset = o.fee.asset_id;
      fee_amount = o.fee.amount;
   }
};

class postgres_indexer_plugin_impl
{
   public:
      explicit postgres_indexer_plugin_impl(postgres_indexer_plugin& _plugin)
         : _self(_plugin)
      {}

      ~postgres_indexer_plugin_impl()
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

      // --- PostgreSQL infrastructure ---
      bool connect_to_postgres();
      bool create_tables();
      std::string escape_string(const std::string& input);
      bool execute_sql(const std::string& sql);
      PGresult* execute_query(const std::string& sql);
      void flush_bulk_buffer();

      // --- Operation history (from elasticsearch) ---
      bool update_account_histories(const signed_block& b);
      void checkState(const fc::time_point_sec& block_time);
      void getOperationType(const optional<operation_history_object>& oho);
      void doOperationHistory(const optional<operation_history_object>& oho);
      void doBlock(uint32_t trx_in_block, const signed_block& b);
      void doVisitor(const optional<operation_history_object>& oho);
      bool add_to_postgres(const account_id_type account_id,
                           const optional<operation_history_object>& oho,
                           const uint32_t block_number);
      const account_transaction_history_object& addNewEntry(
         const account_statistics_object& stats_obj,
         const account_id_type& account_id,
         const optional<operation_history_object>& oho);
      const account_statistics_object& getStatsObject(const account_id_type& account_id);
      void growStats(const account_statistics_object& stats_obj,
                     const account_transaction_history_object& ath);
      void cleanObjects(const account_transaction_history_id_type& ath,
                        const account_id_type& account_id);
      void createInsertLine(const account_transaction_history_object& ath);

      // --- Blockchain objects (from es_objects) ---
      bool genesis();
      bool index_database(const vector<object_id_type>& ids, std::string action);
      void remove_object_from_pg(object_id_type id, const std::string& table_name);
      template<typename T>
      void upsert_object(const T& blockchain_object, const std::string& table_name);

      // --- Content cards & permissions (from postgres_content) ---
      void on_block_content(const signed_block& b);
      std::string get_object_id_from_result(const operation_result& result);
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
      void handle_room_create(const room_create_operation& op,
                               uint32_t block_num, fc::time_point_sec block_time,
                               const std::string& trx_id, const std::string& object_id);
      void handle_room_update(const room_update_operation& op,
                               uint32_t block_num, fc::time_point_sec block_time,
                               const std::string& trx_id);
      void handle_room_add_participant(const room_add_participant_operation& op,
                                        uint32_t block_num, fc::time_point_sec block_time,
                                        const std::string& trx_id, const std::string& object_id);
      void handle_room_remove_participant(const room_remove_participant_operation& op,
                                           uint32_t block_num, fc::time_point_sec block_time,
                                           const std::string& trx_id);
      void handle_room_rotate_key(const room_rotate_key_operation& op,
                                   uint32_t block_num, fc::time_point_sec block_time,
                                   const std::string& trx_id);

      // --- Query API ---
      operation_history_object pg_get_operation_by_id(operation_history_id_type id);
      vector<operation_history_object> pg_get_account_history(
         const account_id_type account_id,
         operation_history_id_type stop,
         unsigned limit,
         operation_history_id_type start);

      // --- State ---
      postgres_indexer_plugin& _self;
      PGconn* pg_conn = nullptr;
      primary_index<operation_history_index>* _oho_index = nullptr;

      // Config
      std::string _postgres_url;
      uint32_t _bulk_replay = 10000;
      uint32_t _bulk_sync = 100;
      bool _visitor = false;
      bool _operation_object = true;
      bool _operation_string = false;
      uint32_t _start_after_block = 0;
      mode _mode = mode::only_save;
      uint32_t _content_start_block = 0;

      // Object type toggles
      bool _index_proposals = true;
      bool _index_accounts = true;
      bool _index_assets = true;
      bool _index_balances = true;
      bool _index_limit_orders = false;
      bool _index_bitassets = true;
      bool _keep_only_current = true;

      // Runtime state
      bool is_sync = false;
      uint32_t limit_documents = 0;
      std::vector<std::string> bulk_sql_buffer;

      // Current operation data (reused across calls within a block)
      int16_t op_type = 0;
      struct {
         uint16_t trx_in_block = 0;
         uint16_t op_in_trx = 0;
         std::string operation_result;
         uint32_t virtual_op = 0;
         std::string op_string;
         std::string op_object_json;
      } current_op;

      struct {
         uint32_t block_num = 0;
         fc::time_point_sec block_time;
         std::string trx_id;
      } current_block;

      struct {
         std::string fee_asset;
         std::string fee_asset_name;
         int64_t fee_amount = 0;
         double fee_amount_units = 0;
         std::string transfer_asset;
         std::string transfer_asset_name;
         int64_t transfer_amount = 0;
         double transfer_amount_units = 0;
         std::string transfer_from;
         std::string transfer_to;
         std::string fill_order_id;
         std::string fill_account_id;
         std::string fill_pays_asset_id;
         std::string fill_pays_asset_name;
         int64_t fill_pays_amount = 0;
         double fill_pays_amount_units = 0;
         std::string fill_receives_asset_id;
         std::string fill_receives_asset_name;
         int64_t fill_receives_amount = 0;
         double fill_receives_amount_units = 0;
         double fill_price = 0;
         double fill_price_units = 0;
         bool fill_is_maker = false;
      } current_visitor;

      // Object indexing state
      uint32_t obj_block_number = 0;
      fc::time_point_sec obj_block_time;
};

// ============================================================================
// PostgreSQL Infrastructure
// ============================================================================

std::string postgres_indexer_plugin_impl::escape_string(const std::string& input)
{
   if (!pg_conn) return input;

   char* escaped = PQescapeLiteral(pg_conn, input.c_str(), input.length());
   if (!escaped) return "''";

   std::string result(escaped);
   PQfreemem(escaped);
   return result;
}

bool postgres_indexer_plugin_impl::execute_sql(const std::string& sql)
{
   if (!pg_conn) return false;

   PGresult* res = PQexec(pg_conn, sql.c_str());
   ExecStatusType status = PQresultStatus(res);

   bool success = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);

   if (!success) {
      elog("PostgreSQL error: ${e}", ("e", PQerrorMessage(pg_conn)));
      elog("SQL: ${s}", ("s", sql.substr(0, 500)));
   }

   PQclear(res);
   return success;
}

PGresult* postgres_indexer_plugin_impl::execute_query(const std::string& sql)
{
   if (!pg_conn) return nullptr;

   PGresult* res = PQexec(pg_conn, sql.c_str());
   ExecStatusType status = PQresultStatus(res);

   if (status != PGRES_TUPLES_OK) {
      elog("PostgreSQL query error: ${e}", ("e", PQerrorMessage(pg_conn)));
      PQclear(res);
      return nullptr;
   }

   return res;
}

bool postgres_indexer_plugin_impl::connect_to_postgres()
{
   pg_conn = PQconnectdb(_postgres_url.c_str());

   if (PQstatus(pg_conn) != CONNECTION_OK) {
      elog("PostgreSQL connection failed: ${e}", ("e", PQerrorMessage(pg_conn)));
      PQfinish(pg_conn);
      pg_conn = nullptr;
      return false;
   }

   ilog("postgres_indexer: PostgreSQL connection successful");
   return true;
}

bool postgres_indexer_plugin_impl::create_tables()
{
   const std::string sql = R"(

      -- Operation history (replaces elasticsearch plugin)
      CREATE TABLE IF NOT EXISTS indexer_operation_history (
         id                      BIGSERIAL PRIMARY KEY,
         account_id              VARCHAR(32) NOT NULL,
         operation_id            VARCHAR(32) NOT NULL,
         operation_id_num        BIGINT NOT NULL,
         sequence                BIGINT NOT NULL,
         trx_in_block            INTEGER NOT NULL,
         op_in_trx               INTEGER NOT NULL,
         operation_result        TEXT NOT NULL,
         virtual_op              INTEGER NOT NULL DEFAULT 0,
         op_type                 SMALLINT NOT NULL,
         op_object               JSONB,
         op_string               TEXT,
         block_num               BIGINT NOT NULL,
         block_time              TIMESTAMP NOT NULL,
         trx_id                  VARCHAR(64),
         fee_asset               VARCHAR(32),
         fee_asset_name          VARCHAR(32),
         fee_amount              BIGINT,
         fee_amount_units        DOUBLE PRECISION,
         transfer_asset          VARCHAR(32),
         transfer_asset_name     VARCHAR(32),
         transfer_amount         BIGINT,
         transfer_amount_units   DOUBLE PRECISION,
         transfer_from           VARCHAR(32),
         transfer_to             VARCHAR(32),
         fill_order_id           VARCHAR(32),
         fill_account_id         VARCHAR(32),
         fill_pays_asset_id      VARCHAR(32),
         fill_pays_asset_name    VARCHAR(32),
         fill_pays_amount        BIGINT,
         fill_pays_amount_units  DOUBLE PRECISION,
         fill_receives_asset_id  VARCHAR(32),
         fill_receives_asset_name VARCHAR(32),
         fill_receives_amount    BIGINT,
         fill_receives_amount_units DOUBLE PRECISION,
         fill_price              DOUBLE PRECISION,
         fill_price_units        DOUBLE PRECISION,
         fill_is_maker           BOOLEAN,
         created_at              TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );

      CREATE INDEX IF NOT EXISTS idx_oh_account_id ON indexer_operation_history(account_id);
      CREATE INDEX IF NOT EXISTS idx_oh_account_op ON indexer_operation_history(account_id, operation_id_num DESC);
      CREATE INDEX IF NOT EXISTS idx_oh_operation_id ON indexer_operation_history(operation_id);
      CREATE INDEX IF NOT EXISTS idx_oh_block_num ON indexer_operation_history(block_num);
      CREATE UNIQUE INDEX IF NOT EXISTS idx_oh_account_seq ON indexer_operation_history(account_id, sequence);

      -- Blockchain object tables (replaces es_objects plugin)
      CREATE TABLE IF NOT EXISTS indexer_proposals (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_prop_block_num ON indexer_proposals(block_num);
      CREATE INDEX IF NOT EXISTS idx_prop_object_id ON indexer_proposals(object_id);

      CREATE TABLE IF NOT EXISTS indexer_accounts (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         name        VARCHAR(64),
         memo_key    VARCHAR(128),
         referrer    VARCHAR(32),
         registrar   VARCHAR(32),
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_acc_block_num ON indexer_accounts(block_num);
      CREATE INDEX IF NOT EXISTS idx_acc_object_id ON indexer_accounts(object_id);
      CREATE INDEX IF NOT EXISTS idx_acc_name ON indexer_accounts(name);

      CREATE TABLE IF NOT EXISTS indexer_assets (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         symbol      VARCHAR(32),
         issuer      VARCHAR(32),
         precision   SMALLINT,
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_asset_block_num ON indexer_assets(block_num);
      CREATE INDEX IF NOT EXISTS idx_asset_object_id ON indexer_assets(object_id);
      CREATE INDEX IF NOT EXISTS idx_asset_symbol ON indexer_assets(symbol);

      CREATE TABLE IF NOT EXISTS indexer_balances (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         owner       VARCHAR(32),
         asset_type  VARCHAR(32),
         balance     BIGINT,
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_bal_block_num ON indexer_balances(block_num);
      CREATE INDEX IF NOT EXISTS idx_bal_object_id ON indexer_balances(object_id);
      CREATE INDEX IF NOT EXISTS idx_bal_owner ON indexer_balances(owner);
      CREATE INDEX IF NOT EXISTS idx_bal_asset_type ON indexer_balances(asset_type);

      CREATE TABLE IF NOT EXISTS indexer_limit_orders (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_lo_block_num ON indexer_limit_orders(block_num);
      CREATE INDEX IF NOT EXISTS idx_lo_object_id ON indexer_limit_orders(object_id);

      CREATE TABLE IF NOT EXISTS indexer_bitassets (
         id          BIGSERIAL PRIMARY KEY,
         object_id   VARCHAR(32) NOT NULL,
         data        JSONB NOT NULL,
         block_num   BIGINT NOT NULL,
         block_time  TIMESTAMP NOT NULL,
         created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      CREATE INDEX IF NOT EXISTS idx_ba_block_num ON indexer_bitassets(block_num);
      CREATE INDEX IF NOT EXISTS idx_ba_object_id ON indexer_bitassets(object_id);

      -- Content cards and permissions (from postgres_content plugin)
      CREATE TABLE IF NOT EXISTS indexer_content_cards (
         id                  SERIAL PRIMARY KEY,
         content_card_id     VARCHAR(32) NOT NULL,
         subject_account     VARCHAR(32) NOT NULL,
         hash                VARCHAR(256),
         url                 TEXT,
         type                VARCHAR(64),
         description         TEXT,
         content_key         TEXT,
         storage_data        TEXT,
         file_name           TEXT,
         file_size           BIGINT,
         room_id             VARCHAR(32),
         key_epoch           INTEGER NOT NULL DEFAULT 0,
         block_num           BIGINT NOT NULL,
         block_time          TIMESTAMP NOT NULL,
         trx_id              VARCHAR(64),
         operation_type      SMALLINT NOT NULL,
         is_removed          BOOLEAN DEFAULT FALSE,
         created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(content_card_id)
      );

      CREATE INDEX IF NOT EXISTS idx_cc_subject ON indexer_content_cards(subject_account);
      CREATE INDEX IF NOT EXISTS idx_cc_block_time ON indexer_content_cards(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_cc_type ON indexer_content_cards(type);
      CREATE INDEX IF NOT EXISTS idx_cc_is_removed ON indexer_content_cards(is_removed);
      CREATE INDEX IF NOT EXISTS idx_cc_file_name ON indexer_content_cards(file_name);
      CREATE INDEX IF NOT EXISTS idx_cc_file_size ON indexer_content_cards(file_size);
      CREATE INDEX IF NOT EXISTS idx_cc_room ON indexer_content_cards(room_id);

      CREATE TABLE IF NOT EXISTS indexer_permissions (
         id                  SERIAL PRIMARY KEY,
         permission_id       VARCHAR(32) NOT NULL,
         subject_account     VARCHAR(32) NOT NULL,
         operator_account    VARCHAR(32) NOT NULL,
         permission_type     VARCHAR(64),
         object_id           VARCHAR(32),
         content_key         TEXT,
         block_num           BIGINT NOT NULL,
         block_time          TIMESTAMP NOT NULL,
         trx_id              VARCHAR(64),
         operation_type      SMALLINT NOT NULL,
         is_removed          BOOLEAN DEFAULT FALSE,
         created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(permission_id)
      );

      CREATE INDEX IF NOT EXISTS idx_perm_subject ON indexer_permissions(subject_account);
      CREATE INDEX IF NOT EXISTS idx_perm_operator ON indexer_permissions(operator_account);
      CREATE INDEX IF NOT EXISTS idx_perm_object ON indexer_permissions(object_id);
      CREATE INDEX IF NOT EXISTS idx_perm_block_time ON indexer_permissions(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_perm_is_removed ON indexer_permissions(is_removed);

      -- Rooms (encrypted threads)
      CREATE TABLE IF NOT EXISTS indexer_rooms (
         id                  SERIAL PRIMARY KEY,
         room_id             VARCHAR(32) NOT NULL,
         owner               VARCHAR(32) NOT NULL,
         name                VARCHAR(256),
         room_key            TEXT,
         current_epoch       INTEGER NOT NULL DEFAULT 0,
         block_num           BIGINT NOT NULL,
         block_time          TIMESTAMP NOT NULL,
         trx_id              VARCHAR(64),
         operation_type      SMALLINT NOT NULL,
         created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(room_id)
      );

      CREATE INDEX IF NOT EXISTS idx_room_owner ON indexer_rooms(owner);
      CREATE INDEX IF NOT EXISTS idx_room_block_time ON indexer_rooms(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_room_name ON indexer_rooms(name);

      -- Room participants
      CREATE TABLE IF NOT EXISTS indexer_room_participants (
         id                  SERIAL PRIMARY KEY,
         participant_id      VARCHAR(32) NOT NULL,
         room_id             VARCHAR(32) NOT NULL,
         participant         VARCHAR(32) NOT NULL,
         content_key         TEXT,
         block_num           BIGINT NOT NULL,
         block_time          TIMESTAMP NOT NULL,
         trx_id              VARCHAR(64),
         operation_type      SMALLINT NOT NULL,
         is_removed          BOOLEAN DEFAULT FALSE,
         created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(participant_id)
      );

      CREATE INDEX IF NOT EXISTS idx_rp_room ON indexer_room_participants(room_id);
      CREATE INDEX IF NOT EXISTS idx_rp_participant ON indexer_room_participants(participant);
      CREATE INDEX IF NOT EXISTS idx_rp_block_time ON indexer_room_participants(block_time DESC);
      CREATE INDEX IF NOT EXISTS idx_rp_is_removed ON indexer_room_participants(is_removed);

      -- Room key epochs (per-participant per-epoch encrypted keys)
      CREATE TABLE IF NOT EXISTS indexer_room_key_epochs (
         id                  SERIAL PRIMARY KEY,
         room_id             VARCHAR(32) NOT NULL,
         epoch               INTEGER NOT NULL,
         participant         VARCHAR(32) NOT NULL,
         encrypted_key       TEXT NOT NULL,
         block_num           BIGINT NOT NULL,
         block_time          TIMESTAMP NOT NULL,
         created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
         UNIQUE(room_id, epoch, participant)
      );

      CREATE INDEX IF NOT EXISTS idx_rke_room ON indexer_room_key_epochs(room_id);
      CREATE INDEX IF NOT EXISTS idx_rke_participant ON indexer_room_key_epochs(participant);
      CREATE INDEX IF NOT EXISTS idx_rke_room_participant ON indexer_room_key_epochs(room_id, participant);

      -- Migrations for existing deployments
      ALTER TABLE indexer_rooms ADD COLUMN IF NOT EXISTS current_epoch INTEGER NOT NULL DEFAULT 0;
      ALTER TABLE indexer_content_cards ADD COLUMN IF NOT EXISTS key_epoch INTEGER NOT NULL DEFAULT 0;

      -- Sync state bookkeeping
      CREATE TABLE IF NOT EXISTS indexer_sync_state (
         id              SERIAL PRIMARY KEY,
         last_block_num  BIGINT NOT NULL DEFAULT 0,
         last_block_time TIMESTAMP,
         updated_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
      );
      INSERT INTO indexer_sync_state (id, last_block_num) VALUES (1, 0) ON CONFLICT (id) DO NOTHING;

   )";

   if (!execute_sql(sql)) {
      elog("Failed to create tables");
      return false;
   }

   // Add UNIQUE constraints on object tables only when keeping current state
   if (_keep_only_current) {
      const std::string unique_sql = R"(
         DO $$ BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_prop_object_id') THEN
               ALTER TABLE indexer_proposals ADD CONSTRAINT uq_prop_object_id UNIQUE (object_id);
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_acc_object_id') THEN
               ALTER TABLE indexer_accounts ADD CONSTRAINT uq_acc_object_id UNIQUE (object_id);
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_asset_object_id') THEN
               ALTER TABLE indexer_assets ADD CONSTRAINT uq_asset_object_id UNIQUE (object_id);
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_bal_object_id') THEN
               ALTER TABLE indexer_balances ADD CONSTRAINT uq_bal_object_id UNIQUE (object_id);
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_lo_object_id') THEN
               ALTER TABLE indexer_limit_orders ADD CONSTRAINT uq_lo_object_id UNIQUE (object_id);
            END IF;
            IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'uq_ba_object_id') THEN
               ALTER TABLE indexer_bitassets ADD CONSTRAINT uq_ba_object_id UNIQUE (object_id);
            END IF;
         END $$;
      )";
      if (!execute_sql(unique_sql)) {
         elog("Failed to create UNIQUE constraints for object tables");
         return false;
      }
   }

   ilog("postgres_indexer: PostgreSQL tables created/verified");
   return true;
}

void postgres_indexer_plugin_impl::flush_bulk_buffer()
{
   if (bulk_sql_buffer.empty()) return;

   std::string combined = "BEGIN;\n";
   for (const auto& sql : bulk_sql_buffer)
   {
      combined += sql + ";\n";
   }

   // Update sync state with current block info
   if (current_block.block_num > 0) {
      combined += "UPDATE indexer_sync_state SET last_block_num = "
         + std::to_string(current_block.block_num)
         + ", last_block_time = to_timestamp("
         + std::to_string(current_block.block_time.sec_since_epoch())
         + "), updated_at = CURRENT_TIMESTAMP WHERE id = 1;\n";
   }

   combined += "COMMIT;";

   if (!execute_sql(combined))
   {
      elog("Bulk flush failed for ${n} statements", ("n", bulk_sql_buffer.size()));
      execute_sql("ROLLBACK;");
      FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
         "Error in bulk flush to PostgreSQL.");
   }

   bulk_sql_buffer.clear();
}

// ============================================================================
// Operation History (ported from elasticsearch plugin)
// ============================================================================

void postgres_indexer_plugin_impl::checkState(const fc::time_point_sec& block_time)
{
   if ((fc::time_point::now() - block_time) < fc::seconds(30))
   {
      limit_documents = _bulk_sync;
      is_sync = true;
   }
   else
   {
      limit_documents = _bulk_replay;
      is_sync = false;
   }
}

void postgres_indexer_plugin_impl::getOperationType(const optional<operation_history_object>& oho)
{
   if (!oho->id.is_null())
      op_type = oho->op.which();
}

void postgres_indexer_plugin_impl::doOperationHistory(const optional<operation_history_object>& oho)
{
   current_op.trx_in_block = oho->trx_in_block;
   current_op.op_in_trx = oho->op_in_trx;
   current_op.operation_result = fc::json::to_string(oho->result);
   current_op.virtual_op = oho->virtual_op;

   if (_operation_object) {
      variant op_object;
      oho->op.visit(fc::from_static_variant(op_object, FC_PACK_MAX_DEPTH));
      current_op.op_object_json = fc::json::to_string(op_object, fc::json::legacy_generator);
   }
   if (_operation_string)
      current_op.op_string = fc::json::to_string(oho->op);
}

void postgres_indexer_plugin_impl::doBlock(uint32_t trx_in_block, const signed_block& b)
{
   std::string trx_id = "";
   if (trx_in_block < b.transactions.size())
      trx_id = b.transactions[trx_in_block].id().str();
   current_block.block_num = b.block_num();
   current_block.block_time = b.timestamp;
   current_block.trx_id = trx_id;
}

void postgres_indexer_plugin_impl::doVisitor(const optional<operation_history_object>& oho)
{
   graphene::chain::database& db = database();

   pg_operation_visitor o_v;
   oho->op.visit(o_v);

   // Fee data
   auto fee_asset_obj = o_v.fee_asset(db);
   current_visitor.fee_asset = std::string(object_id_type(o_v.fee_asset));
   current_visitor.fee_asset_name = fee_asset_obj.symbol;
   current_visitor.fee_amount = o_v.fee_amount.value;
   current_visitor.fee_amount_units = o_v.fee_amount.value /
      (double)asset::scaled_precision(fee_asset_obj.precision).value;

   // Transfer data
   auto transfer_asset_obj = o_v.transfer_asset_id(db);
   current_visitor.transfer_asset = std::string(object_id_type(o_v.transfer_asset_id));
   current_visitor.transfer_asset_name = transfer_asset_obj.symbol;
   current_visitor.transfer_amount = o_v.transfer_amount.value;
   current_visitor.transfer_amount_units = o_v.transfer_amount.value /
      (double)asset::scaled_precision(transfer_asset_obj.precision).value;
   current_visitor.transfer_from = std::string(object_id_type(o_v.transfer_from));
   current_visitor.transfer_to = std::string(object_id_type(o_v.transfer_to));

   // Fill order data
   auto fill_pays_asset_obj = o_v.fill_pays_asset_id(db);
   auto fill_receives_asset_obj = o_v.fill_receives_asset_id(db);
   current_visitor.fill_order_id = std::string(object_id_type(o_v.fill_order_id));
   current_visitor.fill_account_id = std::string(object_id_type(o_v.fill_account_id));
   current_visitor.fill_pays_asset_id = std::string(object_id_type(o_v.fill_pays_asset_id));
   current_visitor.fill_pays_asset_name = fill_pays_asset_obj.symbol;
   current_visitor.fill_pays_amount = o_v.fill_pays_amount.value;
   current_visitor.fill_pays_amount_units = o_v.fill_pays_amount.value /
      (double)asset::scaled_precision(fill_pays_asset_obj.precision).value;
   current_visitor.fill_receives_asset_id = std::string(object_id_type(o_v.fill_receives_asset_id));
   current_visitor.fill_receives_asset_name = fill_receives_asset_obj.symbol;
   current_visitor.fill_receives_amount = o_v.fill_receives_amount.value;
   current_visitor.fill_receives_amount_units = o_v.fill_receives_amount.value /
      (double)asset::scaled_precision(fill_receives_asset_obj.precision).value;
   current_visitor.fill_price = o_v.fill_fill_price;
   double fill_pays_units = o_v.fill_pays_amount.value /
      (double)asset::scaled_precision(fill_pays_asset_obj.precision).value;
   if (fill_pays_units > 0)
      current_visitor.fill_price_units = (o_v.fill_receives_amount.value /
         (double)asset::scaled_precision(fill_receives_asset_obj.precision).value) / fill_pays_units;
   else
      current_visitor.fill_price_units = 0;
   current_visitor.fill_is_maker = o_v.fill_is_maker;
}

const account_statistics_object& postgres_indexer_plugin_impl::getStatsObject(const account_id_type& account_id)
{
   graphene::chain::database& db = database();
   return db.get_account_stats_by_owner(account_id);
}

const account_transaction_history_object& postgres_indexer_plugin_impl::addNewEntry(
   const account_statistics_object& stats_obj,
   const account_id_type& account_id,
   const optional<operation_history_object>& oho)
{
   graphene::chain::database& db = database();
   const auto& ath = db.create<account_transaction_history_object>([&](account_transaction_history_object& obj) {
      obj.operation_id = oho->id;
      obj.account = account_id;
      obj.sequence = stats_obj.total_ops + 1;
      obj.next = stats_obj.most_recent_op;
   });
   return ath;
}

void postgres_indexer_plugin_impl::growStats(const account_statistics_object& stats_obj,
                                              const account_transaction_history_object& ath)
{
   graphene::chain::database& db = database();
   db.modify(stats_obj, [&](account_statistics_object& obj) {
      obj.most_recent_op = ath.id;
      obj.total_ops = ath.sequence;
   });
}

void postgres_indexer_plugin_impl::cleanObjects(const account_transaction_history_id_type& ath_id,
                                                 const account_id_type& account_id)
{
   graphene::chain::database& db = database();
   const auto& his_idx = db.get_index_type<account_transaction_history_index>();
   const auto& by_seq_idx = his_idx.indices().get<by_seq>();
   auto itr = by_seq_idx.lower_bound(boost::make_tuple(account_id, 0));
   if (itr != by_seq_idx.end() && itr->account == account_id && itr->id != ath_id) {
      const auto remove_op_id = itr->operation_id;
      const auto itr_remove = itr;
      ++itr;
      db.remove(*itr_remove);
      if (itr != by_seq_idx.end() && itr->account == account_id)
      {
         db.modify(*itr, [&](account_transaction_history_object& obj) {
            obj.next = account_transaction_history_id_type();
         });
      }
      const auto& by_opid_idx = his_idx.indices().get<by_opid>();
      if (by_opid_idx.find(remove_op_id) == by_opid_idx.end()) {
         db.remove(remove_op_id(db));
      }
   }
}

void postgres_indexer_plugin_impl::createInsertLine(const account_transaction_history_object& ath)
{
   std::string account_id_str = std::string(object_id_type(ath.account));
   std::string operation_id_str = std::string(object_id_type(ath.operation_id));
   int64_t operation_id_num = ath.operation_id.instance.value;
   int64_t sequence = ath.sequence;

   std::string sql = "INSERT INTO indexer_operation_history "
      "(account_id, operation_id, operation_id_num, sequence, trx_in_block, op_in_trx, "
      "operation_result, virtual_op, op_type, op_object, op_string, "
      "block_num, block_time, trx_id";

   if (_visitor) {
      sql += ", fee_asset, fee_asset_name, fee_amount, fee_amount_units"
             ", transfer_asset, transfer_asset_name, transfer_amount, transfer_amount_units"
             ", transfer_from, transfer_to"
             ", fill_order_id, fill_account_id"
             ", fill_pays_asset_id, fill_pays_asset_name, fill_pays_amount, fill_pays_amount_units"
             ", fill_receives_asset_id, fill_receives_asset_name, fill_receives_amount, fill_receives_amount_units"
             ", fill_price, fill_price_units, fill_is_maker";
   }

   sql += ") VALUES ("
      + escape_string(account_id_str) + ", "
      + escape_string(operation_id_str) + ", "
      + std::to_string(operation_id_num) + ", "
      + std::to_string(sequence) + ", "
      + std::to_string(current_op.trx_in_block) + ", "
      + std::to_string(current_op.op_in_trx) + ", "
      + escape_string(current_op.operation_result) + ", "
      + std::to_string(current_op.virtual_op) + ", "
      + std::to_string(op_type) + ", ";

   if (_operation_object && !current_op.op_object_json.empty())
      sql += escape_string(current_op.op_object_json) + "::jsonb, ";
   else
      sql += "NULL, ";

   if (_operation_string && !current_op.op_string.empty())
      sql += escape_string(current_op.op_string) + ", ";
   else
      sql += "NULL, ";

   sql += std::to_string(current_block.block_num) + ", "
      "to_timestamp(" + std::to_string(current_block.block_time.sec_since_epoch()) + "), "
      + escape_string(current_block.trx_id);

   if (_visitor) {
      sql += ", " + escape_string(current_visitor.fee_asset)
         + ", " + escape_string(current_visitor.fee_asset_name)
         + ", " + std::to_string(current_visitor.fee_amount)
         + ", " + std::to_string(current_visitor.fee_amount_units)
         + ", " + escape_string(current_visitor.transfer_asset)
         + ", " + escape_string(current_visitor.transfer_asset_name)
         + ", " + std::to_string(current_visitor.transfer_amount)
         + ", " + std::to_string(current_visitor.transfer_amount_units)
         + ", " + escape_string(current_visitor.transfer_from)
         + ", " + escape_string(current_visitor.transfer_to)
         + ", " + escape_string(current_visitor.fill_order_id)
         + ", " + escape_string(current_visitor.fill_account_id)
         + ", " + escape_string(current_visitor.fill_pays_asset_id)
         + ", " + escape_string(current_visitor.fill_pays_asset_name)
         + ", " + std::to_string(current_visitor.fill_pays_amount)
         + ", " + std::to_string(current_visitor.fill_pays_amount_units)
         + ", " + escape_string(current_visitor.fill_receives_asset_id)
         + ", " + escape_string(current_visitor.fill_receives_asset_name)
         + ", " + std::to_string(current_visitor.fill_receives_amount)
         + ", " + std::to_string(current_visitor.fill_receives_amount_units)
         + ", " + std::to_string(current_visitor.fill_price)
         + ", " + std::to_string(current_visitor.fill_price_units)
         + ", " + (current_visitor.fill_is_maker ? "TRUE" : "FALSE");
   }

   sql += ") ON CONFLICT (account_id, sequence) DO NOTHING";

   bulk_sql_buffer.push_back(sql);
}

bool postgres_indexer_plugin_impl::add_to_postgres(const account_id_type account_id,
                                                    const optional<operation_history_object>& oho,
                                                    const uint32_t block_number)
{
   const auto& stats_obj = getStatsObject(account_id);
   const auto& ath = addNewEntry(stats_obj, account_id, oho);
   growStats(stats_obj, ath);

   if (block_number > _start_after_block) {
      createInsertLine(ath);
   }
   cleanObjects(ath.id, account_id);

   if (bulk_sql_buffer.size() >= limit_documents) {
      try {
         flush_bulk_buffer();
      } catch (...) {
         return false;
      }
   }

   return true;
}

bool postgres_indexer_plugin_impl::update_account_histories(const signed_block& b)
{
   checkState(b.timestamp);

   graphene::chain::database& db = database();
   const vector<optional<operation_history_object>>& hist = db.get_applied_operations();
   bool is_first = true;
   auto skip_oho_id = [&is_first, &db, this]() {
      if (is_first && db._undo_db.enabled())
      {
         db.remove(db.create<operation_history_object>([](operation_history_object& obj) {}));
         is_first = false;
      }
      else
         _oho_index->use_next_id();
   };

   for (const optional<operation_history_object>& o_op : hist) {
      optional<operation_history_object> oho;

      auto create_oho = [&]() {
         is_first = false;
         return optional<operation_history_object>(
            db.create<operation_history_object>([&](operation_history_object& h) {
               if (o_op.valid())
               {
                  h.op           = o_op->op;
                  h.result       = o_op->result;
                  h.block_num    = o_op->block_num;
                  h.trx_in_block = o_op->trx_in_block;
                  h.op_in_trx    = o_op->op_in_trx;
                  h.virtual_op   = o_op->virtual_op;
               }
            }));
      };

      if (!o_op.valid()) {
         skip_oho_id();
         continue;
      }
      oho = create_oho();

      // Populate operation data
      getOperationType(oho);
      doOperationHistory(oho);
      doBlock(oho->trx_in_block, b);
      if (_visitor)
         doVisitor(oho);

      const operation_history_object& op = *o_op;

      // Get the set of accounts this operation applies to
      flat_set<account_id_type> impacted;
      vector<authority> other;
      operation_get_required_authorities(op.op, impacted, impacted, other, false);

      if (op.op.is_type<account_create_operation>())
         impacted.insert(op.result.get<object_id_type>());
      else
         operation_get_impacted_accounts(op.op, impacted, false);

      if (op.result.is_type<extendable_operation_result>())
      {
         const auto& op_result = op.result.get<extendable_operation_result>();
         if (op_result.value.impacted_accounts.valid())
         {
            for (const auto& a : *op_result.value.impacted_accounts)
               impacted.insert(a);
         }
      }

      for (auto& a : other)
         for (auto& item : a.account_auths)
            impacted.insert(item.first);

      for (auto& account_id : impacted)
      {
         if (!add_to_postgres(account_id, oho, b.block_num()))
         {
            elog("Error adding data to PostgreSQL: block num ${b}, account ${a}",
                 ("b", b.block_num())("a", account_id));
            return false;
         }
      }
   }

   // Flush at end of block when in sync mode
   if (is_sync && !bulk_sql_buffer.empty())
   {
      try {
         flush_bulk_buffer();
      } catch (...) {
         return false;
      }
   }

   return true;
}

// ============================================================================
// Blockchain Objects (ported from es_objects plugin)
// ============================================================================

template<typename T>
void postgres_indexer_plugin_impl::upsert_object(const T& blockchain_object, const std::string& table_name)
{
   fc::variant blockchain_object_variant;
   fc::to_variant(blockchain_object, blockchain_object_variant, GRAPHENE_NET_MAX_NESTED_OBJECTS);
   std::string data = fc::json::to_string(blockchain_object_variant, fc::json::legacy_generator);
   std::string obj_id = std::string(blockchain_object.id);

   if (_keep_only_current) {
      std::string sql = "INSERT INTO indexer_" + table_name + " "
         "(object_id, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (object_id) DO UPDATE SET "
         "data = EXCLUDED.data, block_num = EXCLUDED.block_num, block_time = EXCLUDED.block_time";
      bulk_sql_buffer.push_back(sql);
   } else {
      // History mode: insert new row each time (no UNIQUE constraint on object_id)
      std::string sql = "INSERT INTO indexer_" + table_name + " "
         "(object_id, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + "))";
      bulk_sql_buffer.push_back(sql);
   }
}

template<>
void postgres_indexer_plugin_impl::upsert_object<account_object>(const account_object& obj, const std::string& table_name)
{
   fc::variant v;
   fc::to_variant(obj, v, GRAPHENE_NET_MAX_NESTED_OBJECTS);
   std::string data = fc::json::to_string(v, fc::json::legacy_generator);
   std::string obj_id = std::string(obj.id);
   std::string name = obj.name;
   std::string memo_key = std::string(obj.options.memo_key);
   std::string referrer = std::string(object_id_type(obj.referrer));
   std::string registrar = std::string(object_id_type(obj.registrar));

   if (_keep_only_current) {
      std::string sql = "INSERT INTO indexer_accounts "
         "(object_id, name, memo_key, referrer, registrar, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(name) + ", "
         + escape_string(memo_key) + ", "
         + escape_string(referrer) + ", "
         + escape_string(registrar) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (object_id) DO UPDATE SET "
         "name = EXCLUDED.name, memo_key = EXCLUDED.memo_key, "
         "referrer = EXCLUDED.referrer, registrar = EXCLUDED.registrar, "
         "data = EXCLUDED.data, block_num = EXCLUDED.block_num, block_time = EXCLUDED.block_time";
      bulk_sql_buffer.push_back(sql);
   } else {
      std::string sql = "INSERT INTO indexer_accounts "
         "(object_id, name, memo_key, referrer, registrar, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(name) + ", "
         + escape_string(memo_key) + ", "
         + escape_string(referrer) + ", "
         + escape_string(registrar) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + "))";
      bulk_sql_buffer.push_back(sql);
   }
}

template<>
void postgres_indexer_plugin_impl::upsert_object<asset_object>(const asset_object& obj, const std::string& table_name)
{
   fc::variant v;
   fc::to_variant(obj, v, GRAPHENE_NET_MAX_NESTED_OBJECTS);
   std::string data = fc::json::to_string(v, fc::json::legacy_generator);
   std::string obj_id = std::string(obj.id);
   std::string symbol = obj.symbol;
   std::string issuer = std::string(object_id_type(obj.issuer));
   int precision = obj.precision;

   if (_keep_only_current) {
      std::string sql = "INSERT INTO indexer_assets "
         "(object_id, symbol, issuer, precision, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(symbol) + ", "
         + escape_string(issuer) + ", "
         + std::to_string(precision) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (object_id) DO UPDATE SET "
         "symbol = EXCLUDED.symbol, issuer = EXCLUDED.issuer, precision = EXCLUDED.precision, "
         "data = EXCLUDED.data, block_num = EXCLUDED.block_num, block_time = EXCLUDED.block_time";
      bulk_sql_buffer.push_back(sql);
   } else {
      std::string sql = "INSERT INTO indexer_assets "
         "(object_id, symbol, issuer, precision, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(symbol) + ", "
         + escape_string(issuer) + ", "
         + std::to_string(precision) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + "))";
      bulk_sql_buffer.push_back(sql);
   }
}

template<>
void postgres_indexer_plugin_impl::upsert_object<account_balance_object>(const account_balance_object& obj, const std::string& table_name)
{
   fc::variant v;
   fc::to_variant(obj, v, GRAPHENE_NET_MAX_NESTED_OBJECTS);
   std::string data = fc::json::to_string(v, fc::json::legacy_generator);
   std::string obj_id = std::string(obj.id);
   std::string owner = std::string(object_id_type(obj.owner));
   std::string asset_type = std::string(object_id_type(obj.asset_type));
   int64_t balance = obj.balance.value;

   if (_keep_only_current) {
      std::string sql = "INSERT INTO indexer_balances "
         "(object_id, owner, asset_type, balance, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(owner) + ", "
         + escape_string(asset_type) + ", "
         + std::to_string(balance) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (object_id) DO UPDATE SET "
         "owner = EXCLUDED.owner, asset_type = EXCLUDED.asset_type, balance = EXCLUDED.balance, "
         "data = EXCLUDED.data, block_num = EXCLUDED.block_num, block_time = EXCLUDED.block_time";
      bulk_sql_buffer.push_back(sql);
   } else {
      std::string sql = "INSERT INTO indexer_balances "
         "(object_id, owner, asset_type, balance, data, block_num, block_time) VALUES ("
         + escape_string(obj_id) + ", "
         + escape_string(owner) + ", "
         + escape_string(asset_type) + ", "
         + std::to_string(balance) + ", "
         + escape_string(data) + "::jsonb, "
         + std::to_string(obj_block_number) + ", "
         "to_timestamp(" + std::to_string(obj_block_time.sec_since_epoch()) + "))";
      bulk_sql_buffer.push_back(sql);
   }
}

void postgres_indexer_plugin_impl::remove_object_from_pg(object_id_type id, const std::string& table_name)
{
   if (_keep_only_current)
   {
      std::string sql = "DELETE FROM indexer_" + table_name
         + " WHERE object_id = " + escape_string(std::string(id));
      bulk_sql_buffer.push_back(sql);
   }
}

bool postgres_indexer_plugin_impl::genesis()
{
   ilog("postgres_indexer: inserting data from genesis");

   graphene::chain::database& db = _self.database();

   obj_block_number = db.head_block_num();
   obj_block_time = db.head_block_time();

   if (_index_accounts) {
      auto& index_accounts = db.get_index(1, 2);
      index_accounts.inspect_all_objects([this, &db](const graphene::db::object& o) {
         auto obj = db.find_object(o.id);
         auto a = static_cast<const account_object*>(obj);
         upsert_object<account_object>(*a, "accounts");
      });
   }
   if (_index_assets) {
      auto& index_assets = db.get_index(1, 3);
      index_assets.inspect_all_objects([this, &db](const graphene::db::object& o) {
         auto obj = db.find_object(o.id);
         auto a = static_cast<const asset_object*>(obj);
         upsert_object<asset_object>(*a, "assets");
      });
   }
   if (_index_balances) {
      auto& index_balances = db.get_index(2, 5);
      index_balances.inspect_all_objects([this, &db](const graphene::db::object& o) {
         auto obj = db.find_object(o.id);
         auto b = static_cast<const account_balance_object*>(obj);
         upsert_object<account_balance_object>(*b, "balances");
      });
   }

   // Flush genesis data
   try {
      flush_bulk_buffer();
   } catch (...) {
      FC_THROW_EXCEPTION(graphene::chain::plugin_exception, "Error inserting genesis data.");
   }

   return true;
}

bool postgres_indexer_plugin_impl::index_database(const vector<object_id_type>& ids, std::string action)
{
   graphene::chain::database& db = _self.database();

   obj_block_time = db.head_block_time();
   obj_block_number = db.head_block_num();

   if (obj_block_number > _start_after_block) {

      uint32_t obj_limit_documents = 0;
      if ((fc::time_point::now() - obj_block_time) < fc::seconds(30))
         obj_limit_documents = _bulk_sync;
      else
         obj_limit_documents = _bulk_replay;

      for (auto const& value : ids) {
         if (value.is<proposal_object>() && _index_proposals) {
            auto obj = db.find_object(value);
            auto p = static_cast<const proposal_object*>(obj);
            if (p != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(p->id, "proposals");
               else
                  upsert_object<proposal_object>(*p, "proposals");
            }
         } else if (value.is<account_object>() && _index_accounts) {
            auto obj = db.find_object(value);
            auto a = static_cast<const account_object*>(obj);
            if (a != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(a->id, "accounts");
               else
                  upsert_object<account_object>(*a, "accounts");
            }
         } else if (value.is<asset_object>() && _index_assets) {
            auto obj = db.find_object(value);
            auto a = static_cast<const asset_object*>(obj);
            if (a != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(a->id, "assets");
               else
                  upsert_object<asset_object>(*a, "assets");
            }
         } else if (value.is<account_balance_object>() && _index_balances) {
            auto obj = db.find_object(value);
            auto b = static_cast<const account_balance_object*>(obj);
            if (b != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(b->id, "balances");
               else
                  upsert_object<account_balance_object>(*b, "balances");
            }
         } else if (value.is<limit_order_object>() && _index_limit_orders) {
            auto obj = db.find_object(value);
            auto l = static_cast<const limit_order_object*>(obj);
            if (l != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(l->id, "limit_orders");
               else
                  upsert_object<limit_order_object>(*l, "limit_orders");
            }
         } else if (value.is<asset_bitasset_data_object>() && _index_bitassets) {
            auto obj = db.find_object(value);
            auto ba = static_cast<const asset_bitasset_data_object*>(obj);
            if (ba != nullptr) {
               if (action == "delete")
                  remove_object_from_pg(ba->id, "bitassets");
               else
                  upsert_object<asset_bitasset_data_object>(*ba, "bitassets");
            }
         }
      }

      if (bulk_sql_buffer.size() >= obj_limit_documents) {
         try {
            flush_bulk_buffer();
         } catch (...) {
            return false;
         }
      }
   }

   return true;
}

// ============================================================================
// Content Cards & Permissions (ported from postgres_content plugin)
// ============================================================================

std::string postgres_indexer_plugin_impl::get_object_id_from_result(const operation_result& result)
{
   if (result.which() == 1) {
      return std::string(result.get<object_id_type>());
   }
   return "";
}

void postgres_indexer_plugin_impl::on_block_content(const signed_block& b)
{
   if (!pg_conn) return;

   uint32_t block_num = b.block_num();
   if (block_num < _content_start_block) return;

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

      std::string created_object_id = get_object_id_from_result(result);
      int op_type_val = op.which();

      if (op_type_val == 41) {
         handle_content_card_create(op.get<content_card_create_operation>(),
                                     block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type_val == 42) {
         handle_content_card_update(op.get<content_card_update_operation>(),
                                     block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type_val == 43) {
         handle_content_card_remove(op.get<content_card_remove_operation>(),
                                     block_num, b.timestamp, trx_id);
      }
      else if (op_type_val == 44) {
         handle_permission_create(op.get<permission_create_operation>(),
                                   block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type_val == 45) {
         handle_permission_remove(op.get<permission_remove_operation>(),
                                   block_num, b.timestamp, trx_id);
      }
      else if (op_type_val == 64) {
         flat_set<object_id_type> new_objects;
         if (result.which() == 3) {
            new_objects = result.get<generic_operation_result>().new_objects;
         }
         handle_permission_create_many(op.get<permission_create_many_operation>(),
                                   block_num, b.timestamp, trx_id, new_objects);
      }
      else if (op_type_val == 65) {
         handle_room_create(op.get<room_create_operation>(),
                            block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type_val == 66) {
         handle_room_update(op.get<room_update_operation>(),
                            block_num, b.timestamp, trx_id);
      }
      else if (op_type_val == 67) {
         handle_room_add_participant(op.get<room_add_participant_operation>(),
                                      block_num, b.timestamp, trx_id, created_object_id);
      }
      else if (op_type_val == 68) {
         handle_room_remove_participant(op.get<room_remove_participant_operation>(),
                                         block_num, b.timestamp, trx_id);
      }
      else if (op_type_val == 69) {
         handle_room_rotate_key(op.get<room_rotate_key_operation>(),
                                 block_num, b.timestamp, trx_id);
      }
   }
}

void postgres_indexer_plugin_impl::handle_content_card_create(
   const content_card_create_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));
   std::string content_card_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string file_name_val = "NULL";
   std::string file_size_val = "NULL";
   try {
      fc::variant v = fc::json::from_string(op.storage_data);
      if (v.is_object()) {
         fc::variant_object obj = v.get_object();
         if (obj.contains("file_name"))
            file_name_val = escape_string(obj["file_name"].as_string());
         if (obj.contains("file_size"))
            file_size_val = std::to_string(obj["file_size"].as_uint64());
      }
   } catch (...) {}

   std::string room_id_val = "NULL";
   uint32_t key_epoch_val = 0;
   if (op.room.valid()) {
      room_id_val = escape_string(std::string(object_id_type(*op.room)));
      try { key_epoch_val = database().get(*op.room).current_epoch; } catch (...) {}
   }

   std::string sql = "INSERT INTO indexer_content_cards "
      "(content_card_id, subject_account, hash, url, type, description, content_key, storage_data, "
      "file_name, file_size, room_id, "
      "key_epoch, block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(content_card_id) + ", "
      + escape_string(subject_account) + ", "
      + escape_string(op.hash) + ", "
      + escape_string(op.url) + ", "
      + escape_string(op.type) + ", "
      + escape_string(op.description) + ", "
      + escape_string(op.content_key) + ", "
      + escape_string(op.storage_data) + ", "
      + file_name_val + ", "
      + file_size_val + ", "
      + room_id_val + ", "
      + std::to_string(key_epoch_val) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "41, FALSE) "
      "ON CONFLICT (content_card_id) DO UPDATE SET "
      "hash = EXCLUDED.hash, url = EXCLUDED.url, type = EXCLUDED.type, "
      "description = EXCLUDED.description, content_key = EXCLUDED.content_key, "
      "storage_data = EXCLUDED.storage_data, file_name = EXCLUDED.file_name, "
      "file_size = EXCLUDED.file_size, room_id = EXCLUDED.room_id, key_epoch = EXCLUDED.key_epoch";

   if (!execute_sql(sql)) {
      elog("Failed to insert content_card_create: block ${b}", ("b", block_num));
   }
}

void postgres_indexer_plugin_impl::handle_content_card_update(
   const content_card_update_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string subject_account = std::string(object_id_type(op.subject_account));
   std::string content_card_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string file_name_val = "NULL";
   std::string file_size_val = "NULL";
   try {
      fc::variant v = fc::json::from_string(op.storage_data);
      if (v.is_object()) {
         fc::variant_object obj = v.get_object();
         if (obj.contains("file_name"))
            file_name_val = escape_string(obj["file_name"].as_string());
         if (obj.contains("file_size"))
            file_size_val = std::to_string(obj["file_size"].as_uint64());
      }
   } catch (...) {}

   std::string room_id_val = "NULL";
   uint32_t key_epoch_val = 0;
   if (op.room.valid()) {
      room_id_val = escape_string(std::string(object_id_type(*op.room)));
      try { key_epoch_val = database().get(*op.room).current_epoch; } catch (...) {}
   }

   std::string sql = "INSERT INTO indexer_content_cards "
      "(content_card_id, subject_account, hash, url, type, description, content_key, storage_data, "
      "file_name, file_size, room_id, "
      "key_epoch, block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(content_card_id) + ", "
      + escape_string(subject_account) + ", "
      + escape_string(op.hash) + ", "
      + escape_string(op.url) + ", "
      + escape_string(op.type) + ", "
      + escape_string(op.description) + ", "
      + escape_string(op.content_key) + ", "
      + escape_string(op.storage_data) + ", "
      + file_name_val + ", "
      + file_size_val + ", "
      + room_id_val + ", "
      + std::to_string(key_epoch_val) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "42, FALSE) "
      "ON CONFLICT (content_card_id) DO UPDATE SET "
      "hash = EXCLUDED.hash, url = EXCLUDED.url, type = EXCLUDED.type, "
      "description = EXCLUDED.description, content_key = EXCLUDED.content_key, "
      "storage_data = EXCLUDED.storage_data, file_name = EXCLUDED.file_name, "
      "file_size = EXCLUDED.file_size, room_id = EXCLUDED.room_id, key_epoch = EXCLUDED.key_epoch, "
      "block_num = EXCLUDED.block_num, "
      "block_time = EXCLUDED.block_time, operation_type = 42";

   if (!execute_sql(sql)) {
      elog("Failed to insert content_card_update: block ${b}", ("b", block_num));
   }
}

void postgres_indexer_plugin_impl::handle_content_card_remove(
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
   }
}

void postgres_indexer_plugin_impl::handle_permission_create(
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
   }
}

void postgres_indexer_plugin_impl::handle_permission_create_many(
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
      }
   }
}

void postgres_indexer_plugin_impl::handle_permission_remove(
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
   }
}

void postgres_indexer_plugin_impl::handle_room_create(
   const room_create_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string owner = std::string(object_id_type(op.owner));
   std::string room_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string sql = "INSERT INTO indexer_rooms "
      "(room_id, owner, name, room_key, current_epoch, block_num, block_time, trx_id, operation_type) VALUES ("
      + escape_string(room_id) + ", "
      + escape_string(owner) + ", "
      + escape_string(op.name) + ", "
      + escape_string(op.room_key) + ", "
      "0, "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "65) "
      "ON CONFLICT (room_id) DO UPDATE SET "
      "name = EXCLUDED.name, room_key = EXCLUDED.room_key, current_epoch = EXCLUDED.current_epoch";

   if (!execute_sql(sql)) {
      elog("Failed to insert room_create: block ${b}", ("b", block_num));
   }

   // Create epoch 0 record for the owner
   std::string epoch_sql = "INSERT INTO indexer_room_key_epochs "
      "(room_id, epoch, participant, encrypted_key, block_num, block_time) VALUES ("
      + escape_string(room_id) + ", 0, "
      + escape_string(owner) + ", "
      + escape_string(op.room_key) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + ")) "
      "ON CONFLICT (room_id, epoch, participant) DO UPDATE SET encrypted_key = EXCLUDED.encrypted_key";

   if (!execute_sql(epoch_sql)) {
      elog("Failed to insert room_create epoch 0: block ${b}", ("b", block_num));
   }
}

void postgres_indexer_plugin_impl::handle_room_update(
   const room_update_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id)
{
   std::string room_id = std::string(object_id_type(op.room));

   std::string sql = "UPDATE indexer_rooms SET "
      "name = " + escape_string(op.name) + ", "
      "block_num = " + std::to_string(block_num) + ", "
      "block_time = to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      "operation_type = 66 "
      "WHERE room_id = " + escape_string(room_id);

   if (!execute_sql(sql)) {
      elog("Failed to update room_update: block ${b}", ("b", block_num));
   }
}

void postgres_indexer_plugin_impl::handle_room_add_participant(
   const room_add_participant_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id,
   const std::string& object_id)
{
   std::string room_id = std::string(object_id_type(op.room));
   std::string participant = std::string(object_id_type(op.participant));
   std::string participant_id = object_id.empty() ? ("pending-" + trx_id) : object_id;

   std::string sql = "INSERT INTO indexer_room_participants "
      "(participant_id, room_id, participant, content_key, block_num, block_time, trx_id, operation_type, is_removed) VALUES ("
      + escape_string(participant_id) + ", "
      + escape_string(room_id) + ", "
      + escape_string(participant) + ", "
      + escape_string(op.content_key) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      + escape_string(trx_id) + ", "
      "67, FALSE) "
      "ON CONFLICT (participant_id) DO UPDATE SET "
      "content_key = EXCLUDED.content_key, is_removed = FALSE";

   if (!execute_sql(sql)) {
      elog("Failed to insert room_add_participant: block ${b}", ("b", block_num));
   }

   // Insert current epoch key record
   uint32_t current_epoch = 0;
   try { current_epoch = database().get(op.room).current_epoch; } catch (...) {}

   std::string epoch_sql = "INSERT INTO indexer_room_key_epochs "
      "(room_id, epoch, participant, encrypted_key, block_num, block_time) VALUES ("
      + escape_string(room_id) + ", "
      + std::to_string(current_epoch) + ", "
      + escape_string(participant) + ", "
      + escape_string(op.content_key) + ", "
      + std::to_string(block_num) + ", "
      "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + ")) "
      "ON CONFLICT (room_id, epoch, participant) DO UPDATE SET encrypted_key = EXCLUDED.encrypted_key";

   if (!execute_sql(epoch_sql)) {
      elog("Failed to insert room_add_participant epoch key: block ${b}", ("b", block_num));
   }

   // Insert historical epoch keys if provided
   for (const auto& ek : op.epoch_keys) {
      std::string hist_sql = "INSERT INTO indexer_room_key_epochs "
         "(room_id, epoch, participant, encrypted_key, block_num, block_time) VALUES ("
         + escape_string(room_id) + ", "
         + std::to_string(ek.first) + ", "
         + escape_string(participant) + ", "
         + escape_string(ek.second) + ", "
         + std::to_string(block_num) + ", "
         "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (room_id, epoch, participant) DO UPDATE SET encrypted_key = EXCLUDED.encrypted_key";

      if (!execute_sql(hist_sql)) {
         elog("Failed to insert room_add_participant historical epoch key: block ${b}", ("b", block_num));
      }
   }
}

void postgres_indexer_plugin_impl::handle_room_remove_participant(
   const room_remove_participant_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id)
{
   std::string participant_id = std::string(object_id_type(op.participant_id));

   std::string sql = "UPDATE indexer_room_participants SET "
      "is_removed = TRUE, "
      "block_num = " + std::to_string(block_num) + ", "
      "block_time = to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      "operation_type = 68 "
      "WHERE participant_id = " + escape_string(participant_id);

   if (!execute_sql(sql)) {
      elog("Failed to update room_remove_participant: block ${b}", ("b", block_num));
   }
}

void postgres_indexer_plugin_impl::handle_room_rotate_key(
   const room_rotate_key_operation& op,
   uint32_t block_num, fc::time_point_sec block_time, const std::string& trx_id)
{
   std::string room_id = std::string(object_id_type(op.room));
   std::string owner = std::string(object_id_type(op.owner));

   // Get the new epoch from chain state (evaluator already incremented it)
   uint32_t new_epoch = 0;
   try { new_epoch = database().get(op.room).current_epoch; } catch (...) {}

   // Update room: new room_key and current_epoch
   std::string sql = "UPDATE indexer_rooms SET "
      "room_key = " + escape_string(op.new_room_key) + ", "
      "current_epoch = " + std::to_string(new_epoch) + ", "
      "block_num = " + std::to_string(block_num) + ", "
      "block_time = to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + "), "
      "operation_type = 69 "
      "WHERE room_id = " + escape_string(room_id);

   if (!execute_sql(sql)) {
      elog("Failed to update room_rotate_key: block ${b}", ("b", block_num));
   }

   // Update each participant's content_key in indexer_room_participants
   for (const auto& pk : op.participant_keys) {
      std::string part_account = std::string(object_id_type(pk.first));

      std::string upd_sql = "UPDATE indexer_room_participants SET "
         "content_key = " + escape_string(pk.second) + " "
         "WHERE room_id = " + escape_string(room_id) + " "
         "AND participant = " + escape_string(part_account) + " "
         "AND is_removed = FALSE";

      if (!execute_sql(upd_sql)) {
         elog("Failed to update participant key during rotate: block ${b}", ("b", block_num));
      }

      // Insert epoch record for each participant
      std::string epoch_sql = "INSERT INTO indexer_room_key_epochs "
         "(room_id, epoch, participant, encrypted_key, block_num, block_time) VALUES ("
         + escape_string(room_id) + ", "
         + std::to_string(new_epoch) + ", "
         + escape_string(part_account) + ", "
         + escape_string(pk.second) + ", "
         + std::to_string(block_num) + ", "
         "to_timestamp(" + std::to_string(block_time.sec_since_epoch()) + ")) "
         "ON CONFLICT (room_id, epoch, participant) DO UPDATE SET encrypted_key = EXCLUDED.encrypted_key";

      if (!execute_sql(epoch_sql)) {
         elog("Failed to insert epoch key during rotate: block ${b}", ("b", block_num));
      }
   }
}

// ============================================================================
// Query API (replaces elasticsearch query methods)
// ============================================================================

operation_history_object postgres_indexer_plugin_impl::pg_get_operation_by_id(operation_history_id_type id)
{
   const string operation_id_string = std::string(object_id_type(id));

   std::string sql = "SELECT operation_id, op_string, operation_result, block_num, "
      "trx_in_block, op_in_trx, virtual_op "
      "FROM indexer_operation_history "
      "WHERE operation_id = " + escape_string(operation_id_string) + " "
      "LIMIT 1";

   PGresult* res = execute_query(sql);
   if (!res || PQntuples(res) == 0) {
      if (res) PQclear(res);
      FC_THROW_EXCEPTION(fc::exception, "Operation not found: ${id}", ("id", operation_id_string));
   }

   try {
      operation_history_object result;

      // Parse operation_id
      std::string op_id_str = PQgetvalue(res, 0, 0);
      fc::variant op_id_var(op_id_str);
      fc::from_variant(op_id_var, result.id, GRAPHENE_MAX_NESTED_OBJECTS);

      // Parse operation
      if (!PQgetisnull(res, 0, 1)) {
         std::string op_str = PQgetvalue(res, 0, 1);
         auto op_var = fc::json::from_string(op_str);
         fc::from_variant(op_var, result.op, GRAPHENE_MAX_NESTED_OBJECTS);
      }

      // Parse result
      if (!PQgetisnull(res, 0, 2)) {
         std::string result_str = PQgetvalue(res, 0, 2);
         auto result_var = fc::json::from_string(result_str);
         fc::from_variant(result_var, result.result, GRAPHENE_MAX_NESTED_OBJECTS);
      }

      result.block_num = std::stoul(PQgetvalue(res, 0, 3));
      result.trx_in_block = std::stoul(PQgetvalue(res, 0, 4));
      result.op_in_trx = std::stoul(PQgetvalue(res, 0, 5));
      result.virtual_op = std::stoul(PQgetvalue(res, 0, 6));

      PQclear(res);
      return result;
   } catch (...) {
      PQclear(res);
      throw;
   }
}

vector<operation_history_object> postgres_indexer_plugin_impl::pg_get_account_history(
   const account_id_type account_id,
   operation_history_id_type stop,
   unsigned limit,
   operation_history_id_type start)
{
   const string account_id_string = std::string(object_id_type(account_id));
   const auto stop_number = stop.instance.value;
   const auto start_number = start.instance.value;

   std::string sql = "SELECT operation_id, op_string, operation_result, block_num, "
      "trx_in_block, op_in_trx, virtual_op "
      "FROM indexer_operation_history "
      "WHERE account_id = " + escape_string(account_id_string);

   if (stop_number == 0)
      sql += " AND operation_id_num >= " + fc::to_string(stop_number)
           + " AND operation_id_num <= " + fc::to_string(start_number);
   else if (stop_number > 0)
      sql += " AND operation_id_num > " + fc::to_string(stop_number)
           + " AND operation_id_num <= " + fc::to_string(start_number);

   sql += " ORDER BY operation_id_num DESC LIMIT " + fc::to_string(limit);

   PGresult* res = execute_query(sql);
   vector<operation_history_object> result;

   if (!res) return result;

   try {
      int nrows = PQntuples(res);
      for (int i = 0; i < nrows; i++)
      {
         operation_history_object obj;

         // Parse operation_id
         std::string op_id_str = PQgetvalue(res, i, 0);
         fc::variant op_id_var(op_id_str);
         fc::from_variant(op_id_var, obj.id, GRAPHENE_MAX_NESTED_OBJECTS);

         // Parse operation
         if (!PQgetisnull(res, i, 1)) {
            std::string op_str = PQgetvalue(res, i, 1);
            auto op_var = fc::json::from_string(op_str);
            fc::from_variant(op_var, obj.op, GRAPHENE_MAX_NESTED_OBJECTS);
         }

         // Parse result
         if (!PQgetisnull(res, i, 2)) {
            std::string result_str = PQgetvalue(res, i, 2);
            auto result_var = fc::json::from_string(result_str);
            fc::from_variant(result_var, obj.result, GRAPHENE_MAX_NESTED_OBJECTS);
         }

         obj.block_num = std::stoul(PQgetvalue(res, i, 3));
         obj.trx_in_block = std::stoul(PQgetvalue(res, i, 4));
         obj.op_in_trx = std::stoul(PQgetvalue(res, i, 5));
         obj.virtual_op = std::stoul(PQgetvalue(res, i, 6));

         result.push_back(obj);
      }

      PQclear(res);
      return result;
   } catch (...) {
      PQclear(res);
      throw;
   }
}

} // end namespace detail

// ============================================================================
// Plugin Public Interface
// ============================================================================

postgres_indexer_plugin::postgres_indexer_plugin(graphene::app::application& app) :
   plugin(app),
   my(std::make_unique<detail::postgres_indexer_plugin_impl>(*this))
{
}

postgres_indexer_plugin::~postgres_indexer_plugin() = default;

std::string postgres_indexer_plugin::plugin_name() const
{
   return "postgres_indexer";
}

std::string postgres_indexer_plugin::plugin_description() const
{
   return "Unified PostgreSQL indexer for operation history, blockchain objects, "
          "content cards and permissions.";
}

void postgres_indexer_plugin::plugin_set_program_options(
   boost::program_options::options_description& cli,
   boost::program_options::options_description& cfg)
{
   cli.add_options()
      ("postgres-indexer-url", boost::program_options::value<std::string>(),
         "PostgreSQL connection URL (e.g., postgresql://user:pass@localhost/dbname)")
      ("postgres-indexer-bulk-replay", boost::program_options::value<uint32_t>(),
         "Number of bulk documents to index on replay (default: 10000)")
      ("postgres-indexer-bulk-sync", boost::program_options::value<uint32_t>(),
         "Number of bulk documents to index on a synchronized chain (default: 100)")
      ("postgres-indexer-visitor", boost::program_options::value<bool>(),
         "Index additional fee/transfer/fill visitor data (default: false)")
      ("postgres-indexer-operation-object", boost::program_options::value<bool>(),
         "Store operation as JSONB object (default: true)")
      ("postgres-indexer-operation-string", boost::program_options::value<bool>(),
         "Store operation as string, needed for query mode (default: false)")
      ("postgres-indexer-start-after-block", boost::program_options::value<uint32_t>(),
         "Start indexing after this block number (default: 0)")
      ("postgres-indexer-mode", boost::program_options::value<uint16_t>(),
         "Mode: 0=only_save, 1=only_query, 2=all (default: 0)")
      ("postgres-indexer-proposals", boost::program_options::value<bool>(),
         "Index proposal objects (default: true)")
      ("postgres-indexer-accounts", boost::program_options::value<bool>(),
         "Index account objects (default: true)")
      ("postgres-indexer-assets", boost::program_options::value<bool>(),
         "Index asset objects (default: true)")
      ("postgres-indexer-balances", boost::program_options::value<bool>(),
         "Index balance objects (default: true)")
      ("postgres-indexer-limit-orders", boost::program_options::value<bool>(),
         "Index limit order objects (default: false)")
      ("postgres-indexer-bitassets", boost::program_options::value<bool>(),
         "Index bitasset data (default: true)")
      ("postgres-indexer-keep-only-current", boost::program_options::value<bool>(),
         "Keep only current state of objects (default: true)")
      ("postgres-indexer-content-start-block", boost::program_options::value<uint32_t>(),
         "Start content card/permission indexing from this block (default: 0)")
      ;
   cfg.add(cli);
}

void postgres_indexer_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{
   ilog("postgres_indexer: plugin_initialize()");

   if (options.count("postgres-indexer-url") > 0) {
      my->_postgres_url = options["postgres-indexer-url"].as<std::string>();
   } else {
      wlog("postgres_indexer: No --postgres-indexer-url specified, plugin will be disabled");
      return;
   }

   // Parse all options
   if (options.count("postgres-indexer-bulk-replay") > 0)
      my->_bulk_replay = options["postgres-indexer-bulk-replay"].as<uint32_t>();
   if (options.count("postgres-indexer-bulk-sync") > 0)
      my->_bulk_sync = options["postgres-indexer-bulk-sync"].as<uint32_t>();
   if (options.count("postgres-indexer-visitor") > 0)
      my->_visitor = options["postgres-indexer-visitor"].as<bool>();
   if (options.count("postgres-indexer-operation-object") > 0)
      my->_operation_object = options["postgres-indexer-operation-object"].as<bool>();
   if (options.count("postgres-indexer-operation-string") > 0)
      my->_operation_string = options["postgres-indexer-operation-string"].as<bool>();
   if (options.count("postgres-indexer-start-after-block") > 0)
      my->_start_after_block = options["postgres-indexer-start-after-block"].as<uint32_t>();
   if (options.count("postgres-indexer-mode") > 0) {
      const auto option_number = options["postgres-indexer-mode"].as<uint16_t>();
      if (option_number > mode::all)
         FC_THROW_EXCEPTION(graphene::chain::plugin_exception, "postgres_indexer mode not valid");
      my->_mode = static_cast<mode>(option_number);
   }
   if (options.count("postgres-indexer-proposals") > 0)
      my->_index_proposals = options["postgres-indexer-proposals"].as<bool>();
   if (options.count("postgres-indexer-accounts") > 0)
      my->_index_accounts = options["postgres-indexer-accounts"].as<bool>();
   if (options.count("postgres-indexer-assets") > 0)
      my->_index_assets = options["postgres-indexer-assets"].as<bool>();
   if (options.count("postgres-indexer-balances") > 0)
      my->_index_balances = options["postgres-indexer-balances"].as<bool>();
   if (options.count("postgres-indexer-limit-orders") > 0)
      my->_index_limit_orders = options["postgres-indexer-limit-orders"].as<bool>();
   if (options.count("postgres-indexer-bitassets") > 0)
      my->_index_bitassets = options["postgres-indexer-bitassets"].as<bool>();
   if (options.count("postgres-indexer-keep-only-current") > 0)
      my->_keep_only_current = options["postgres-indexer-keep-only-current"].as<bool>();
   if (options.count("postgres-indexer-content-start-block") > 0)
      my->_content_start_block = options["postgres-indexer-content-start-block"].as<uint32_t>();

   // Validate mode constraints
   if (my->_mode == mode::all && !my->_operation_string)
      FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
         "If postgres-indexer-mode is set to all then postgres-indexer-operation-string needs to be true");

   // Register indexes for operation history
   my->_oho_index = database().add_index<primary_index<operation_history_index>>();
   database().add_index<primary_index<account_transaction_history_index>>();

   if (my->_mode != mode::only_query) {

      // Signal 1: applied_block - operation history + content + genesis
      database().applied_block.connect([this](const signed_block& b) {
         // Operation history indexing
         if (!my->update_account_histories(b))
            FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
               "Error populating PostgreSQL operation history.");

         // Content cards/permissions indexing
         my->on_block_content(b);

         // Genesis handling for blockchain objects
         if (b.block_num() == 1 && my->_start_after_block == 0) {
            if (!my->genesis())
               FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
                  "Error populating genesis data.");
         }
      });

      // Signal 2: new_objects
      database().new_objects.connect([this](const vector<object_id_type>& ids,
            const flat_set<account_id_type>& impacted_accounts) {
         if (!my->index_database(ids, "create"))
         {
            FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
               "Error creating object in PostgreSQL.");
         }
      });

      // Signal 3: changed_objects
      database().changed_objects.connect([this](const vector<object_id_type>& ids,
            const flat_set<account_id_type>& impacted_accounts) {
         if (!my->index_database(ids, "update"))
         {
            FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
               "Error updating object in PostgreSQL.");
         }
      });

      // Signal 4: removed_objects
      database().removed_objects.connect([this](const vector<object_id_type>& ids,
            const vector<const object*>& objs,
            const flat_set<account_id_type>& impacted_accounts) {
         if (!my->index_database(ids, "delete"))
         {
            FC_THROW_EXCEPTION(graphene::chain::plugin_exception,
               "Error removing object from PostgreSQL.");
         }
      });
   }
}

void postgres_indexer_plugin::plugin_startup()
{
   ilog("postgres_indexer: plugin_startup()");

   if (my->_postgres_url.empty()) {
      wlog("postgres_indexer: Plugin disabled (no URL configured)");
      return;
   }

   if (!my->connect_to_postgres()) {
      FC_THROW_EXCEPTION(fc::exception, "Failed to connect to PostgreSQL at ${url}",
                         ("url", my->_postgres_url));
   }

   if (!my->create_tables()) {
      FC_THROW_EXCEPTION(fc::exception, "Failed to create PostgreSQL tables");
   }

   ilog("postgres_indexer: Plugin started successfully");
}

void postgres_indexer_plugin::plugin_shutdown()
{
   ilog("postgres_indexer: plugin_shutdown()");
   // Flush remaining buffer
   if (!my->bulk_sql_buffer.empty()) {
      try {
         my->flush_bulk_buffer();
      } catch (...) {
         elog("postgres_indexer: Failed to flush remaining buffer on shutdown");
      }
   }
}

// Query API delegators
operation_history_object postgres_indexer_plugin::get_operation_by_id(operation_history_id_type id)
{
   return my->pg_get_operation_by_id(id);
}

vector<operation_history_object> postgres_indexer_plugin::get_account_history(
   const account_id_type account_id,
   operation_history_id_type stop,
   unsigned limit,
   operation_history_id_type start)
{
   return my->pg_get_account_history(account_id, stop, limit, start);
}

mode postgres_indexer_plugin::get_running_mode()
{
   return my->_mode;
}

} } // graphene::postgres_indexer
