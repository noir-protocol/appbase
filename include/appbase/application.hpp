#pragma once
#include <appbase/plugin.hpp>
#include <appbase/channel.hpp>
#include <appbase/method.hpp>
#include <appbase/execution_priority_queue.hpp>
#include <boost/core/demangle.hpp>
#include <filesystem>
#include <typeindex>

namespace appbase {
   namespace fs = std::filesystem;

   class application {
      public:
         /**
          * @brief Get home directory that contains config file and runtime data
          *
          * @return Home directory
          */
         fs::path home_dir() const;

         /**
          * @brief Set home directory for config file and runtime data
          *
          * @param home_dir Home directory
          */
         void set_home_dir(const fs::path& home_dir);

         /**
          * @brief Get config file path
          */
         fs::path config_file() const;

         /**
          * @brief Set config file path
          *
          * @param config_file a filename or path for configuration
          */
         void set_config_file(const fs::path& config_file);

         /** @brief Set function pointer invoked on receipt of SIGHUP
          *
          * The provided function will be invoked on receipt of SIGHUP followed
          * by invoking handle_sighup() on all initialized plugins. Caller
          * is responsible for preserving an object if necessary.
          *
          * @param callback Function pointer that will be invoked when the process
          *                 receives the HUP (1) signal.
          */
          void set_sighup_callback(std::function<void()> callback);

         /**
          * @brief Looks for the --plugin commandline / config option and calls initialize on those plugins
          *
          * @tparam Plugin List of plugins to initalize even if not mentioned by configuration. For plugins started by
          * configuration settings or dependency resolution, this template has no effect.
          * @return true if the application and plugins were initialized, false or exception on error
          */
         template<typename... Plugin>
         bool                 initialize() {
            return initialize_impl({find_plugin<Plugin>()...});
         }

         void                  startup();

         /**
          *  Wait until quit(), SIGINT or SIGTERM and then shutdown.
          *  Should only be executed from one thread.
          */
         void                 exec();
         void                 quit();

         /**
          * If in long running process this flag can be checked to see if processing should be stoppped.
          * @return true if quit() has been called.
          */
         bool                 is_quiting()const;

         static application&  instance();

         abstract_plugin* find_plugin(const std::string& name)const;
         abstract_plugin& get_plugin(const std::string& name)const;

         template<typename Plugin>
         auto& register_plugin() {
            auto existing = find_plugin<Plugin>();
            if(existing)
               return *existing;

            auto plug = new Plugin();
            plugins[plug->name()].reset(plug);
            plug->set_program_options(cli(), config());
            plug->register_dependencies();
            return *plug;
         }

         template<typename Plugin>
         Plugin* find_plugin()const {
            auto name = boost::core::demangle(typeid(Plugin).name());
            return dynamic_cast<Plugin*>(find_plugin(name));
         }

         template<typename Plugin>
         Plugin& get_plugin()const {
            auto ptr = find_plugin<Plugin>();
            return *ptr;
         }

         /**
          * Fetch a reference to the method declared by the passed in type.  This will construct the method
          * on first access.  This allows loose and deferred binding between plugins
          *
          * @tparam MethodDecl - @ref appbase::method_decl
          * @return reference to the method described by the declaration
          */
         template<typename MethodDecl>
         auto get_method() -> std::enable_if_t<is_method_decl<MethodDecl>::value, typename MethodDecl::method_type&> {
            using method_type = typename MethodDecl::method_type;
            auto key = std::type_index(typeid(MethodDecl));
            auto itr = methods.find(key);
            if(itr != methods.end()) {
               return *method_type::get_method(itr->second);
            } else {
               methods.emplace(std::make_pair(key, method_type::make_unique()));
               return  *method_type::get_method(methods.at(key));
            }
         }

         /**
          * Fetch a reference to the channel declared by the passed in type.  This will construct the channel
          * on first access.  This allows loose and deferred binding between plugins
          *
          * @tparam ChannelDecl - @ref appbase::channel_decl
          * @return reference to the channel described by the declaration
          */
         template<typename ChannelDecl>
         auto get_channel() -> std::enable_if_t<is_channel_decl<ChannelDecl>::value, typename ChannelDecl::channel_type&> {
            using channel_type = typename ChannelDecl::channel_type;
            auto key = std::type_index(typeid(ChannelDecl));
            auto itr = channels.find(key);
            if(itr != channels.end()) {
               return *channel_type::get_channel(itr->second);
            } else {
               channels.emplace(std::make_pair(key, channel_type::make_unique()));
               return  *channel_type::get_channel(channels.at(key));
            }
         }

         /**
          * Do not run io_context in any other threads, as application assumes single-threaded execution in exec().
          * @return io_context of application
          */
         boost::asio::io_context& io_context() { return *io_ctx; }

         /**
          * Post func to run on io_context with given priority.
          *
          * @param priority can be appbase::priority::* constants or any int, larger ints run first
          * @param func function to run on io_context
          * @return result of boost::asio::post
          */
         template <typename Func>
         auto post(int priority, Func&& func) {
            return boost::asio::post(*io_ctx, pri_queue.wrap(priority, std::forward<Func>(func)));
         }

         /**
          * Provide access to execution priority queue so it can be used to wrap functions for
          * prioritized execution.
          *
          * Example:
          *   boost::asio::steady_timer timer( app().io_context() );
          *   timer.async_wait( app().get_priority_queue().wrap(priority::low, [](){ do_something(); }) );
          */
         auto& priority_queue() {
            return pri_queue;
         }

         CLI::App& cli();
         CLI::App& config();

         /**
          * Set the current thread schedule priority to maximum.
          * Works for pthreads.
          */
         void set_thread_priority_max();

         template<typename... Plugin>
         int run(int argc, char** argv) {
            try {
               cli().parse(argc, argv);
               if (cli().get_subcommands().empty()) {
                  if (!initialize<Plugin...>()) {
                     return 1;
                  }
                  startup();
                  exec();
               }
               return 0;
            } catch (const CLI::ParseError& e) {
               return cli().exit(e);
            }
         }

      protected:
         template<typename Impl>
         friend class plugin;

         bool initialize_impl(std::vector<abstract_plugin*> autostart_plugins);

         /** these notifications get called from the plugin when their state changes so that
          * the application can call shutdown in the reverse order.
          */
         ///@{
         void plugin_initialized(abstract_plugin& plug){ initialized_plugins.push_back(&plug); }
         void plugin_started(abstract_plugin& plug){ running_plugins.push_back(&plug); }
         ///@}

      private:
         application(); ///< private because application is a singleton that should be accessed via instance()
         std::map<std::string, std::unique_ptr<abstract_plugin>> plugins; ///< all registered plugins
         std::vector<abstract_plugin*>                  initialized_plugins; ///< stored in the order they were started running
         std::vector<abstract_plugin*>                  running_plugins; ///< stored in the order they were started running

         std::function<void()>                          sighup_callback;
         std::map<std::type_index, erased_method_ptr>   methods;
         std::map<std::type_index, erased_channel_ptr>  channels;

         std::shared_ptr<boost::asio::io_context>  io_ctx;
         execution_priority_queue                  pri_queue;

         void start_sighup_handler( std::shared_ptr<boost::asio::signal_set> sighup_set );
         void set_program_options();
         void write_default_config(const fs::path& cfg_file);
         void print_default_config(std::ostream& os);

         void wait_for_signal(std::shared_ptr<boost::asio::signal_set> ss);
         void setup_signal_handling_on_ioc(boost::asio::io_context& ioc, bool startup);

         void shutdown();

         std::unique_ptr<class application_impl> my;

   };

   application& app();

   template<typename Impl>
   class plugin : public abstract_plugin {
      public:
         plugin():_name(boost::core::demangle(typeid(Impl).name())){}

         state get_state()const override         { return _state; }
         const std::string& name()const override { return _name; }

         virtual void register_dependencies() {
            static_cast<Impl*>(this)->plugin_requires([&](auto& plug){});
         }

         void initialize(const CLI::App& cli, const CLI::App& config) override {
            if(_state == registered) {
               _state = initialized;
               static_cast<Impl*>(this)->plugin_requires([&](auto& plug){ plug.initialize(cli, config); });
               static_cast<Impl*>(this)->plugin_initialize(cli, config);
               //ilog( "initializing plugin ${name}", ("name",name()) );
               app().plugin_initialized(*this);
            }
            assert(_state == initialized); /// if initial state was not registered, final state cannot be initialized
         }

         void handle_sighup() override {
         }

         void startup() override {
            if(_state == initialized) {
               _state = started;
               static_cast<Impl*>(this)->plugin_requires([&](auto& plug){ plug.startup(); });
               static_cast<Impl*>(this)->plugin_startup();
               app().plugin_started(*this);
            }
            assert(_state == started); // if initial state was not initialized, final state cannot be started
         }

         void shutdown() override {
            if(_state == started) {
               _state = stopped;
               //ilog( "shutting down plugin ${name}", ("name",name()) );
               static_cast<Impl*>(this)->plugin_shutdown();
            }
         }

      protected:
         plugin(const std::string& name) : _name(name){}

      private:
         state _state = abstract_plugin::registered;
         std::string _name;
   };

   template<typename Data, typename DispatchPolicy>
   void channel<Data,DispatchPolicy>::publish(int priority, const Data& data) {
      if (has_subscribers()) {
         // this will copy data into the lambda
         app().post( priority, [this, data]() {
            _signal(data);
         });
      }
   }

} // namespace appbase
