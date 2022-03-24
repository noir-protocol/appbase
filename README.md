AppBase
--------------

The AppBase library provides a basic framework for building applications from a set of plugins.
AppBase manages the plugin life-cycle and ensures that all plugins are configured, initialized, started and shutdown in the proper order.

## Key Features

- Dynamically Specify Plugins to Load
- Automatically Load Dependent Plugins in Order
- Plugins can specify commandline arguments and configuration file options
- Program gracefully exits from SIGINT, SIGTERM, and SIGPIPE
- Minimal Dependencies (Boost 1.60, c++17)

## Defining a Plugin

Each plugin has a simple life cycle:

1. Initialize - parse configuration file options
1. Startup - start executing, using configuration file options
1. Shutdown - stop everything and free all resources

All plugins must complete the Initialize step before any plugin enters the Startup step.
Any dependent plugin specified by `APPBASE_PLUGIN_REQUIRES` will be Initialized or Started prior to the plugin being Initialized or Started.

Shutdown is called in the reverse order of Startup.

## Boost ASIO

AppBase application owns a `boost::asio::io_context` which starts running when `application::exec()` is called.
If a plugin needs to perform IO or other asynchronous operations then it should dispatch it via `application::io_context()` which is setup to use an execution priority queue.
``` c++
app.post(appbase::priority::low, lambda);
```
OR
``` c++
delay_timer->async_wait(app.get_priority_queue().wrap(priority::low, lambda));
```
Use of `io_context()` directly is not recommended as the priority queue will not be respected.

Because the app calls `io_context::run()` from within `application::exec()` and does not spawn any threads all asynchronous operations posted to the io_context should be run in the same thread.

## Graceful Exit

To trigger a graceful exit, call `application::quit()` or send SIGTERM, SIGINT, or SIGPIPE to the process.

## Dependencies

1. c++17 or newer  (clang or g++)
2. Boost 1.60 or newer compiled with C++17 support

To compile boost with c++17 use:

```
./b2 ... cxxflags="-std=c++0x -stdlib=libc++" linkflags="-stdlib=libc++" ...
```

## Program Options

Program options can be set via commandline arguments or TOML-formatted configuration file.
[CLI11](https://github.com/CLIUtils/CLI11) is used to handle these options.
If path is not given, the default lookup path for configuration file is `$HOME/.APP_NAME/config/CONFIG_FILENAME`.

``` c++
class chain_plugin : public appbase::plugin<chain_plugin> {
public:
  APPBASE_PLUGIN_REQUIRES();

  void set_program_options(CLI::App& config) override {
    // Create subsection `chain` in TOML config file by add_section.
    auto chain_options = config.add_section("chain", "Chain configuration");

    // Can be called with commandline or set by config file.
    chain_options->add_option("--readonly", "open the database in read only mode");

    // Call group("") not to be listed in commandline options. (config file only options)
    chain_options->add_option("--dbsize", "Minimum size MB of database shared memory file")->default_val(8 * 1024ULL)->group("");

    // A command-line option that doesn't have leading dashes `-` is considered
    // as a positional argument. Refer to CLI11 documentation.

    // Call configurable(false) not to be listed in config file. (commandline only options)
    chain_options->add_flag("--replay", "clear chain database and replay all blocks")->configurable(false);
    chain_options->add_flag("--reset", "clear chain database and block log")->configurable(false);
  }

  /* ... */
};
```
