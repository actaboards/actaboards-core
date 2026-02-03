/*
 * Copyright (c) 2024 contributors.
 *
 * The MIT License
 */
#pragma once

#include <graphene/app/plugin.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/operation_history_object.hpp>

namespace graphene { namespace postgres_indexer {

using namespace chain;

#ifndef POSTGRES_INDEXER_SPACE_ID
#define POSTGRES_INDEXER_SPACE_ID 7
#endif

namespace detail
{
   class postgres_indexer_plugin_impl;
}

enum mode { only_save = 0, only_query = 1, all = 2 };

class postgres_indexer_plugin : public graphene::app::plugin
{
   public:
      explicit postgres_indexer_plugin(graphene::app::application& app);
      ~postgres_indexer_plugin() override;

      std::string plugin_name() const override;
      std::string plugin_description() const override;

      void plugin_set_program_options(
         boost::program_options::options_description& cli,
         boost::program_options::options_description& cfg) override;

      void plugin_initialize(const boost::program_options::variables_map& options) override;
      void plugin_startup() override;
      void plugin_shutdown() override;

      // Query API (drop-in replacement for elasticsearch)
      operation_history_object get_operation_by_id(operation_history_id_type id);
      vector<operation_history_object> get_account_history(
         const account_id_type account_id,
         operation_history_id_type stop,
         unsigned limit,
         operation_history_id_type start);
      mode get_running_mode();

      friend class detail::postgres_indexer_plugin_impl;
      std::unique_ptr<detail::postgres_indexer_plugin_impl> my;
};

} } // graphene::postgres_indexer

FC_REFLECT_ENUM( graphene::postgres_indexer::mode, (only_save)(only_query)(all) )
