/*
 * Copyright (c) 2024 contributors.
 *
 * The MIT License
 */
#pragma once

#include <graphene/app/plugin.hpp>
#include <graphene/chain/database.hpp>

namespace graphene { namespace postgres_content {

using namespace chain;

namespace detail
{
   class postgres_content_plugin_impl;
}

class postgres_content_plugin : public graphene::app::plugin
{
   public:
      explicit postgres_content_plugin(graphene::app::application& app);
      ~postgres_content_plugin() override;

      std::string plugin_name() const override;
      std::string plugin_description() const override;

      void plugin_set_program_options(
         boost::program_options::options_description& cli,
         boost::program_options::options_description& cfg) override;

      void plugin_initialize(const boost::program_options::variables_map& options) override;
      void plugin_startup() override;
      void plugin_shutdown() override;

      friend class detail::postgres_content_plugin_impl;
      std::unique_ptr<detail::postgres_content_plugin_impl> my;
};

} } // graphene::postgres_content
