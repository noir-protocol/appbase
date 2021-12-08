#include <appbase/application.hpp>
#include <appbase/version.hpp>

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
      fs::path                data_dir;
      fs::path                config_dir;
      fs::path                config_file;
      fs::path                _logging_conf{"logging.json"};
      fs::path                _config_file_name;

      uint64_t                _version = 0;
      std::string             _version_str = appbase_version_string;
      std::string             _full_version_str = appbase_version_string;

      std::atomic_bool        _is_quiting{false};
};

application::application()
:my(new application_impl()){
   io_serv = std::make_shared<boost::asio::io_service>();
}

application::~application() { }

void application::set_version(uint64_t version) {
  my->_version = version;
}

uint64_t application::version() const {
  return my->_version;
}

std::string application::version_string() const {
   return my->_version_str;
}

void application::set_version_string( std::string v ) {
   my->_version_str = std::move( v );
}

std::string application::full_version_string() const {
   return my->_full_version_str;
}

void application::set_full_version_string( std::string v ) {
   my->_full_version_str = std::move( v );
}

void application::set_default_data_dir(const fs::path& data_dir) {
  my->data_dir = data_dir;
}

void application::set_default_config_dir(const fs::path& config_dir) {
  my->config_dir = config_dir;
}

fs::path application::get_logging_conf() const {
  return my->_logging_conf;
}

void application::wait_for_signal(std::shared_ptr<boost::asio::signal_set> ss) {
   ss->async_wait([this, ss](const boost::system::error_code& ec, int) {
      if(ec)
         return;
      quit();
      wait_for_signal(ss);
   });
}

void application::setup_signal_handling_on_ios(boost::asio::io_service& ios, bool startup) {
   std::shared_ptr<boost::asio::signal_set> ss = std::make_shared<boost::asio::signal_set>(ios, SIGINT, SIGTERM);
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
   boost::asio::io_service startup_thread_ios;
   setup_signal_handling_on_ios(startup_thread_ios, true);
   std::thread startup_thread([&startup_thread_ios]() {
      startup_thread_ios.run();
   });
   auto clean_up_signal_thread = [&startup_thread_ios, &startup_thread]() {
      startup_thread_ios.stop();
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

   //after startup, shut down the signal handling thread and catch the signals back on main io_service
   clean_up_signal_thread();
   setup_signal_handling_on_ios(get_io_service(), false);

#ifdef SIGHUP
   std::shared_ptr<boost::asio::signal_set> sighup_set(new boost::asio::signal_set(get_io_service(), SIGHUP));
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

void application::set_program_options()
{
   for(auto& plug : plugins) {
      plug.second->set_program_options(my->cli, my->config);
   }

   my->config.add_option("plugin", "Plugin(s) to enable, may be specified multiple times")->take_all();

   // dummy options to show help text (not used)
   my->cli.add_option("--home", "Directory containing configuration files and runtime data");
   my->cli.add_option("--config", "Configuration file path");

   my->cli.add_option("--plugin", "Plugin(s) to enable, may be specified multiple times")->take_all();
   my->cli.add_option("--logconf,-l", "Logging configuration file name/path for library users")->default_str("logging.json");
   my->cli.add_flag("--version,-v", "Print version information.");
   my->cli.add_flag("--full-version", "Print full version information.");
   my->cli.add_flag("--print-default-config", "Print default configuration template");
}

bool application::initialize_impl(int argc, char** argv, std::vector<abstract_plugin*> autostart_plugins) {
   set_program_options();

   auto parse_option = [&](const char* option_name) -> std::optional<std::string> {
      auto it = std::find_if(argv, argv + argc, [](const auto v) {
         return std::string(v).find("--home") == 0;
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
   } else {
      my->config_file = config_dir() / "config.toml";
   }
   if (!fs::exists(my->config_file)) {
      write_default_config(my->config_file);
   }
   my->config.set_config("config", my->config_file.generic_string(), "Configuration file path");

   // Parse CLI options
   CLI11_PARSE(my->cli, argc, argv);

   // Parse config.toml only (do not handle CLI arguments)
   CLI11_PARSE(my->config, 1, argv);

   if (my->cli.count("--version")) {
      cout << version_string() << std::endl;
      return false;
   }
   if (my->cli.count("--full-version")) {
      cout << full_version_string() << std::endl;
      return false;
   }
   if (my->cli.count("--print-default-config")) {
      print_default_config(cout);
      return false;
   }

   auto workaround = my->cli["--logconf"]->as<std::string>();
   fs::path logconf = workaround;
   if (logconf.is_relative())
      logconf = config_dir() / logconf;
   my->_logging_conf = logconf;

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
   my->_is_quiting = true;
   io_serv->stop();
}

bool application::is_quiting() const {
   return my->_is_quiting;
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
      boost::asio::io_service::work work(*io_serv);
      (void)work;
      bool more = true;
      while( more || io_serv->run_one() ) {
         while( io_serv->poll_one() ) {}
         // execute the highest priority item
         more = pri_queue.execute_highest();
      }

      shutdown(); /// perform synchronous shutdown
   }
   io_serv.reset();
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

abstract_plugin* application::find_plugin(const std::string& name)const
{
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

fs::path application::data_dir() const {
   if (my->data_dir.empty())
      return home_dir() / "data";
   return my->data_dir;
}

fs::path application::config_dir() const {
   if (my->config_dir.empty())
      return home_dir() / "config";
   return my->config_dir;
}

fs::path application::home_dir() const {
   if (my->home_dir.empty()) {
      auto home_dir = fs::path("." + name());
      auto home = std::getenv("HOME");
      if (home) {
         home_dir = home / home_dir;
      }
      return home_dir;
   }
   return my->home_dir;
}

fs::path application::full_config_file_path() const {
   return fs::canonical(my->_config_file_name);
}

void application::set_sighup_callback(std::function<void()> callback) {
   sighup_callback = callback;
}

const CLI::App& application::get_options() const{
   return my->config;
}

void application::set_name(std::string name) {
   if (my->cli.parsed()) {
      BOOST_THROW_EXCEPTION(std::runtime_error("Can't change app name after parsing options"));
   }
   my->cli.name(name);
}

const std::string& application::name() const {
   return my->cli.get_name();
}

} /// namespace appbase
