#include <appbase/application.hpp>
#include <iostream>
#include <boost/exception/diagnostic_information.hpp>

struct database { };

using std::string;
using std::vector;

class chain_plugin : public appbase::plugin<chain_plugin>
{
   public:
     APPBASE_PLUGIN_REQUIRES();

     void set_program_options( CLI::App& cli, CLI::App& cfg ) override
     {
        auto chain_options = cfg.add_subcommand("chain", "Chain configuration");
        chain_options->configurable();

        chain_options->add_option("readonly", "open the database in read only mode");
        chain_options->add_option("dbsize", "Minimum size MB of database shared memory file")->default_val(8 * 1024ULL);

        cli.add_flag("--replay", "clear chain database and replay all blocks");
        cli.add_flag("--reset", "clear chain database and block log");
     }

     void plugin_initialize( const CLI::App& cli, const CLI::App& cfg ) { std::cout << "initialize chain plugin\n"; }
     void plugin_startup()  { std::cout << "starting chain plugin \n"; }
     void plugin_shutdown() { std::cout << "shutdown chain plugin \n"; }

     database& db() { return _db; }

   private:
     database _db;
};

class net_plugin : public appbase::plugin<net_plugin>
{
   public:
     net_plugin(){};
     ~net_plugin(){};

     APPBASE_PLUGIN_REQUIRES( (chain_plugin) );

     void set_program_options( CLI::App& cli, CLI::App& cfg ) override
     {
        auto net_options = cfg.add_subcommand("net", "Net configuration");
        net_options->configurable();

        net_options->add_option("listen-endpoint", "The local IP address and port to listen for incoming connections.")->default_str("127.0.0.1:9876");
        net_options->add_option("remote-endpoint", "The IP address and port of a remote peer to sync with.")->take_all();
        net_options->add_option("public-endpoint", "The public IP address and port that should be advertized to peers.")->default_str("0.0.0.0:9876");
     }

     void plugin_initialize( const CLI::App& cli, const CLI::App& cfg ) { std::cout << "initialize net plugin\n"; }
     void plugin_startup()  { std::cout << "starting net plugin \n"; }
     void plugin_shutdown() { std::cout << "shutdown net plugin \n"; }
};



int main( int argc, char** argv ) {
   try {
      appbase::app().register_plugin<net_plugin>();
      if( !appbase::app().initialize( argc, argv ) )
         return -1;
      appbase::app().startup();
      appbase::app().exec();
   } catch ( const boost::exception& e ) {
      std::cerr << boost::diagnostic_information(e) << "\n";
   } catch ( const std::exception& e ) {
      std::cerr << e.what() << "\n";
   } catch ( ... ) {
      std::cerr << "unknown exception\n";
   }
   std::cout << "exited cleanly\n";
   return 0;
}