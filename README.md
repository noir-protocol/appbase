AppBase
--------------

The AppBase library provides a basic framework for building applications from
a set of plugins. AppBase manages the plugin life-cycle and ensures that all
plugins are configured, initialized, started, and shutdown in the proper order.

## Key Features

- Dynamically Specify Plugins to Load
- Automatically Load Dependent Plugins in Order
- Plugins can specify commandline arguments and configuration file options
- Program gracefully exits from SIGINT, SIGTERM, and SIGPIPE
- Minimal Dependencies (Boost 1.60, c++17)

## Defining a Plugin

A simple example of a 2-plugin application can be found in the /examples directory. Each plugin has
a simple life cycle:

1. Initialize - parse configuration file options
2. Startup - start executing, using configuration file options
3. Shutdown - stop everything and free all resources

All plugins complete the Initialize step before any plugin enters the Startup step. Any dependent plugin specified
by `APPBASE_PLUGIN_REQUIRES` will be Initialized or Started prior to the plugin being Initialized or Started. 

Shutdown is called in the reverse order of Startup. 

``` c++
class net_plugin : public appbase::plugin<net_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES( (chain_plugin) );

   void set_program_options(CLI::App& config) override {
      auto net_options = config.add_section("net", "Net configuration");

      net_options->add_option("--listen-endpoint", "The local IP address and port to listen for incoming connections.")->default_str("127.0.0.1:9876");
      net_options->add_option("--remote-endpoint", "The IP address and port of a remote peer to sync with.")->take_all();
      net_options->add_option("--public-endpoint", "The public IP address and port that should be advertized to peers.")->default_str("0.0.0.0:9876");
   }

   void plugin_initialize(const CLI::App& config) { std::cout << "initialize net plugin\n"; }
   void plugin_startup()  { std::cout << "starting net plugin \n"; }
   void plugin_shutdown() { std::cout << "shutdown net plugin \n"; }
};



int main(int argc, char** argv) {
   try {
      appbase::app().register_plugin<net_plugin>(); // implicit registration of chain_plugin dependency
      appbase::app().parse_config(argc, argv);
      if (!appbase::app().initialize())
         return -1;
      appbase::app().startup();
      appbase::app().exec();
   } catch (const boost::exception& e) {
      std::cerr << boost::diagnostic_information(e) << "\n";
   } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
   } catch (...) {
      std::cerr << "unknown exception\n";
   }
   std::cout << "exited cleanly\n";
   return 0;
}
```

This example can be used like follows:

```
./examples/appbase_example --plugin net_plugin
initialize chain plugin
initialize net plugin
starting chain plugin
starting net plugin
^C
shutdown net plugin
shutdown chain plugin
exited cleanly
```

### Boost ASIO 

AppBase maintains a singleton `application` instance which can be accessed via `appbase::app()`.  This 
application owns a `boost::asio::io_context` which starts running when `appbase::exec()` is called. If 
a plugin needs to perform IO or other asynchronous operations then it should dispatch it via `application`
`io_context` which is setup to use an execution priority queue.
```
app().post( appbase::priority::low, lambda )
```
OR
```
delay_timer->async_wait( app().get_priority_queue().wrap( priority::low, lambda ) );
```
Use of `io_context()` directly is not recommended as the priority queue will not be respected. 

Because the app calls `io_context::run()` from within `application::exec()` and does not spawn any threads
all asynchronous operations posted to the io_context should be run in the same thread.  

## Graceful Exit 

To trigger a graceful exit call `appbase::app().quit()` or send SIGTERM, SIGINT, or SIGPIPE to the process.

## Dependencies 

1. c++17 or newer  (clang or g++)
2. Boost 1.60 or newer compiled with C++17 support

To compile boost with c++17 use:

```
./b2 ...  cxxflags="-std=c++0x -stdlib=libc++" linkflags="-stdlib=libc++" ...
```

## Program options

Program options can be set by CLI arguments or TOML-formatted configuration
file. AppBase uses [CLI11](https://github.com/CLIUtils/CLI11) to handle these
options. If specific path isn't provided, AppBase looks up configuration file
from `$HOME/.${app_name}/config/${config_filename}`.

``` c++
class chain_plugin : public appbase::plugin<chain_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES();

   void set_program_options(CLI::App& config) override {
      // Create subsection `chain` in TOML config file by add_section.
      auto chain_options = config.add_section("chain", "Chain configuration");

      // Can be called with CLI or set by config file.
      chain_options->add_option("--readonly", "open the database in read only mode");

      // Call group("") not to be listed in CLI options. (Config file only options)
      chain_options->add_option("--dbsize", "Minimum size MB of database shared memory file")->default_val(8 * 1024ULL)->group("");

      // A command-line option that doesn't have leading dashes `-` is considered
      // as a positional argument. Refer to CLI11 documentation.

      // Call configurable(false) not to be listed in config file. (CLI only options)
      chain_options->add_flag("--replay", "clear chain database and replay all blocks")->configurable(false);
      chain_options->add_flag("--reset", "clear chain database and block log")->configurable(false);
   }

   /* ... */
};
```

