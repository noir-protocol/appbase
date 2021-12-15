#include <appbase/application.hpp>

#include <boost/asio/signal_set.hpp>

#include <iostream>
#include <future>
#include <optional>
#include <regex>

namespace appbase {

using std::cout;

class application_impl {
   public:
      CLI::App                cli{"Appbase", "appbase"};
      CLI::App                config;

      fs::path                home_dir;
      fs::path                config_file{"config.toml"};

      std::atomic_bool        is_quiting{false};
};

application::application()
:my(new application_impl()){
   io_ctx = std::make_shared<boost::asio::io_context>();
   set_program_options();
}

fs::path application::home_dir() const {
   if (my->home_dir.empty()) {
      auto home_dir = fs::path("." + my->cli.get_name());
      auto home = std::getenv("HOME");
      if (home) {
         home_dir = home / home_dir;
      }
      return home_dir;
   }
   return my->home_dir;
}

void application::set_home_dir(const fs::path& home_dir) {
   my->home_dir = home_dir;
}

fs::path application::config_file() const {
   if (my->config_file.is_relative())
      return home_dir() / "config" / my->config_file;
   return my->config_file;
}

void application::set_config_file(const fs::path& config_file) {
   my->config_file = config_file;
}

void application::wait_for_signal(std::shared_ptr<boost::asio::signal_set> ss) {
   ss->async_wait([this, ss](const boost::system::error_code& ec, int) {
      if(ec)
         return;
      quit();
      wait_for_signal(ss);
   });
}

void application::setup_signal_handling_on_ioc(boost::asio::io_context& ioc, bool startup) {
   std::shared_ptr<boost::asio::signal_set> ss = std::make_shared<boost::asio::signal_set>(ioc, SIGINT, SIGTERM);
#ifdef SIGPIPE
   ss->add(SIGPIPE);
#endif
#ifdef SIGHUP
   if( startup ) {
      ss->add(SIGHUP);
   }
#endif
   wait_for_signal(ss);
}

void application::startup() {
   //during startup, run a second thread to catch SIGINT/SIGTERM/SIGPIPE/SIGHUP
   boost::asio::io_context startup_thread_ioc;
   setup_signal_handling_on_ioc(startup_thread_ioc, true);
   std::thread startup_thread([&startup_thread_ioc]() {
      startup_thread_ioc.run();
   });
   auto clean_up_signal_thread = [&startup_thread_ioc, &startup_thread]() {
      startup_thread_ioc.stop();
      startup_thread.join();
   };

   try {
      for( auto plugin : initialized_plugins ) {
         if( is_quiting() ) break;
         plugin->startup();
      }

   } catch( ... ) {
      clean_up_signal_thread();
      shutdown();
      throw;
   }

   //after startup, shut down the signal handling thread and catch the signals back on main io_context
   clean_up_signal_thread();
   setup_signal_handling_on_ioc(io_context(), false);

#ifdef SIGHUP
   std::shared_ptr<boost::asio::signal_set> sighup_set(new boost::asio::signal_set(io_context(), SIGHUP));
   start_sighup_handler( sighup_set );
#endif
}

void application::start_sighup_handler( std::shared_ptr<boost::asio::signal_set> sighup_set ) {
#ifdef SIGHUP
   sighup_set->async_wait([sighup_set, this](const boost::system::error_code& err, int /*num*/) {
      if( err ) return;
      app().post(priority::medium, [sighup_set, this]() {
         sighup_callback();
         for( auto plugin : initialized_plugins ) {
            if( is_quiting() ) return;
            plugin->handle_sighup();
         }
      });
      start_sighup_handler( sighup_set );
   });
#endif
}

application& application::instance() {
   static application _app;
   return _app;
}
application& app() { return application::instance(); }

void application::set_program_options() {
   my->config.add_option("plugin", "Plugin(s) to enable, may be specified multiple times")->take_all();

   // dummy options to show help text (not used)
   my->cli.add_option("--home", "Directory containing configuration files and runtime data");
   my->cli.add_option("--config", "Configuration file path");

   my->cli.add_option("--plugin", "Plugin(s) to enable, may be specified multiple times")->take_all();
   my->cli.add_flag("--print-default-config", "Print default configuration template");
}

bool application::initialize_impl(int argc, char** argv, std::vector<abstract_plugin*> autostart_plugins) {
   auto parse_option = [&](const char* option_name) -> std::optional<std::string> {
      auto it = std::find_if(argv, argv + argc, [=](const auto v) {
         return std::string(v).find(option_name) == 0;
      });
      if (it != argv + argc) {
         auto arg = std::string(*it);
         auto pos = arg.find("=");
         if (pos != std::string::npos) {
            arg = arg.substr(pos + 1);
         } else {
            arg = std::string(*++it);
         }
         return arg;
      }
      return std::nullopt;
   };

   // Parse "--home" CLI option
   if (auto arg = parse_option("--home")) {
      fs::path home_dir = *arg;
      if (home_dir.is_relative())
         home_dir = fs::current_path() / home_dir;
      my->home_dir = home_dir;
   }

   // Parse "--config" CLI option
   if (auto arg = parse_option("--config")) {
      my->config_file = *arg;
      if (my->config_file.is_relative())
         my->config_file = fs::current_path() / my->config_file;
   }
   if (!fs::exists(config_file())) {
      write_default_config(config_file());
   }
   my->config.set_config("config", config_file().generic_string(), "Configuration file path");

   // Parse CLI options
   CLI11_PARSE(my->cli, argc, argv);

   // Parse config.toml only (do not handle CLI arguments)
   CLI11_PARSE(my->config, 1, argv);

   if (my->cli.count("--print-default-config")) {
      print_default_config(cout);
      return false;
   }

   // split a string at delimiters
   auto split = [](const auto& str, std::string delim = R"(\s\t,)") -> auto {
      auto re = std::regex(delim);
      return std::vector<std::string>{
         std::sregex_token_iterator(str.begin(), str.end(), re, -1),
         std::sregex_token_iterator()
      };
   };

   if (my->cli.count("--plugin")) {
      auto plugins = my->cli["--plugin"]->as<std::vector<std::string>>();
      for (auto& arg : plugins) {
         for (const std::string& name : split(arg))
            get_plugin(name).initialize(my->cli, my->config);
      }
   }
   if (my->config.count("plugin")) {
      auto plugins = my->config["plugin"]->as<std::vector<std::string>>();
      for (auto& arg : plugins) {
         for (const std::string& name : split(arg))
            get_plugin(name).initialize(my->cli, my->config);
      }
   }
   try {
      for (auto plugin : autostart_plugins)
         if (plugin != nullptr && plugin->get_state() == abstract_plugin::registered)
            plugin->initialize(my->cli, my->config);
   } catch (...) {
      std::cerr << "Failed to initialize\n";
      return false;
   }

   return true;
}

void application::shutdown() {
   for(auto ritr = running_plugins.rbegin();
       ritr != running_plugins.rend(); ++ritr) {
      (*ritr)->shutdown();
   }
   for(auto ritr = running_plugins.rbegin();
       ritr != running_plugins.rend(); ++ritr) {
      plugins.erase((*ritr)->name());
   }
   running_plugins.clear();
   initialized_plugins.clear();
   plugins.clear();
   quit();
}

void application::quit() {
   my->is_quiting = true;
   io_ctx->stop();
}

bool application::is_quiting() const {
   return my->is_quiting;
}

void application::set_thread_priority_max() {
#if __has_include(<pthread.h>)
   pthread_t this_thread = pthread_self();
   struct sched_param params{};
   int policy = 0;
   int ret = pthread_getschedparam(this_thread, &policy, &params);
   if( ret != 0 ) {
      std::cerr << "ERROR: Unable to get thread priority" << std::endl;
   }

   params.sched_priority = sched_get_priority_max(policy);
   ret = pthread_setschedparam(this_thread, policy, &params);
   if( ret != 0 ) {
      std::cerr << "ERROR: Unable to set thread priority" << std::endl;
   }
#endif
}

void application::exec() {
   if (running_plugins.size()) {
      auto work = boost::asio::make_work_guard(*io_ctx);
      (void)work;
      bool more = true;
      while( more || io_ctx->run_one() ) {
         while( io_ctx->poll_one() ) {}
         // execute the highest priority item
         more = pri_queue.execute_highest();
      }

      shutdown(); /// perform synchronous shutdown
   }
   io_ctx.reset();
}

void application::write_default_config(const fs::path& cfg_file) {
   if (!fs::exists(cfg_file.parent_path()))
      fs::create_directories(cfg_file.parent_path());

   std::ofstream out_cfg( fs::path(cfg_file).make_preferred().string());
   print_default_config(out_cfg);
   out_cfg.close();
}

void application::print_default_config(std::ostream& os) {
   os << my->config.config_to_str(true, true) << std::endl;
}

abstract_plugin* application::find_plugin(const std::string& name)const {
   auto itr = plugins.find(name);
   if(itr == plugins.end()) {
      return nullptr;
   }
   return itr->second.get();
}

abstract_plugin& application::get_plugin(const std::string& name)const {
   auto ptr = find_plugin(name);
   if(!ptr)
      BOOST_THROW_EXCEPTION(std::runtime_error("unable to find plugin: " + name));
   return *ptr;
}

void application::set_sighup_callback(std::function<void()> callback) {
   sighup_callback = callback;
}

CLI::App& application::cli() {
   return my->cli;
}

CLI::App& application::config() {
   return my->config;
}

} /// namespace appbase
