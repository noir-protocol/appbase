#include <appbase/application.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>

struct database {};

class chain_plugin : public appbase::plugin<chain_plugin> {
public:
  APPBASE_PLUGIN_REQUIRES();

  void set_program_options(CLI::App& config) override {
    // Create subsection `chain` in TOML config file by add_section.
    auto chain_options = config.add_section("chain", "Chain configuration");

    // Can be called with commandline or set by config file.
    chain_options->add_option("--readonly", "open the database in read only mode");

    // Call group("") not to be listed in commandline options. (config file only options)
    chain_options->add_option("--dbsize", "Minimum size MB of database shared memory file")
      ->default_val(8 * 1024ULL)
      ->group("");

    // A command-line option that doesn't have leading dashes `-` is considered
    // as a positional argument. Refer to CLI11 documentation.

    // Call configurable(false) not to be listed in config file. (commandline only options)
    chain_options->add_flag("--replay", "clear chain database and replay all blocks")->configurable(false);
    chain_options->add_flag("--reset", "clear chain database and block log")->configurable(false);
  }

  void plugin_initialize(const CLI::App& config) {
    std::cout << "initialize chain plugin\n";
  }
  void plugin_startup() {
    std::cout << "starting chain plugin \n";
  }
  void plugin_shutdown() {
    std::cout << "shutdown chain plugin \n";
  }

  database& db() {
    return _db;
  }

private:
  database _db;
};

class net_plugin : public appbase::plugin<net_plugin> {
public:
  APPBASE_PLUGIN_REQUIRES((chain_plugin));

  void set_program_options(CLI::App& config) override {
    auto net_options = config.add_section("net", "Net configuration");

    net_options->add_option("--listen-endpoint", "The local IP address and port to listen for incoming connections.")
      ->default_str("127.0.0.1:9876");
    net_options->add_option("--remote-endpoint", "The IP address and port of a remote peer to sync with.")->take_all();
    net_options->add_option("--public-endpoint", "The public IP address and port that should be advertized to peers.")
      ->default_str("0.0.0.0:9876");
  }

  void plugin_initialize(const CLI::App& config) {
    std::cout << "initialize net plugin\n";
  }
  void plugin_startup() {
    std::cout << "starting net plugin \n";
  }
  void plugin_shutdown() {
    std::cout << "shutdown net plugin \n";
  }
};

int main(int argc, char** argv) {
  try {
    appbase::application app{};
    if (auto arg = std::getenv("HOME")) {
      std::filesystem::path home_dir = arg;
      app.set_home_dir(home_dir / ".app");
    }
    app.set_config_file("app.toml");
    app.register_plugin<net_plugin>();
    app.parse_config(argc, argv);
    if (!app.initialize())
      return -1;
    app.startup();
    app.exec();
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
