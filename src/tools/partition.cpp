#include "partition/partition_config.hpp"
#include "partition/partitioner.hpp"

#include "util/log.hpp"
#include "util/meminfo.hpp"
#include "util/timing_util.hpp"
#include "util/version.hpp"

#include <tbb/task_scheduler_init.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/assert.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <iostream>

using namespace osrm;

enum class return_code : unsigned
{
    ok,
    fail,
    exit
};

return_code parseArguments(int argc, char *argv[], partition::PartitionConfig &config)
{
    // declare a group of options that will be allowed only on command line
    boost::program_options::options_description generic_options("Options");
    generic_options.add_options()("version,v", "Show version")("help,h", "Show this help message");

    // declare a group of options that will be allowed both on command line
    boost::program_options::options_description config_options("Configuration");
    config_options.add_options()
        //
        ("threads,t",
         boost::program_options::value<unsigned int>(&config.requested_num_threads)
             ->default_value(tbb::task_scheduler_init::default_num_threads()),
         "Number of threads to use")
        //
        ("balance",
         boost::program_options::value<double>(&config.balance)->default_value(config.balance),
         "Balance for left and right side in single bisection")
        //
        ("boundary",
         boost::program_options::value<double>(&config.boundary_factor)
             ->default_value(config.boundary_factor),
         "Percentage of embedded nodes to contract as sources and sinks")
        //
        ("optimizing-cuts",
         boost::program_options::value<std::size_t>(&config.num_optimizing_cuts)
             ->default_value(config.num_optimizing_cuts),
         "Number of cuts to use for optimizing a single bisection")
        //
        ("small-component-size",
         boost::program_options::value<std::size_t>(&config.small_component_size)
             ->default_value(config.small_component_size),
         "Size threshold for small components.")
        //
        ("max-cell-sizes",
         boost::program_options::value<std::vector<std::size_t>>(&config.max_cell_sizes)
             ->multitoken()
             ->default_value(config.max_cell_sizes,
                             boost::algorithm::join(
                                 config.max_cell_sizes |
                                     boost::adaptors::transformed(
                                         static_cast<std::string (*)(std::size_t)>(std::to_string)),
                                 " ")),
         "Maximum cell sizes starting from the level 1. The first cell size value is a bisection "
         "termination citerion");

    // hidden options, will be allowed on command line, but will not be
    // shown to the user
    boost::program_options::options_description hidden_options("Hidden options");
    hidden_options.add_options()(
        "input,i",
        boost::program_options::value<boost::filesystem::path>(&config.base_path),
        "Input file in .osrm format");

    // positional option
    boost::program_options::positional_options_description positional_options;
    positional_options.add("input", 1);

    // combine above options for parsing
    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic_options).add(config_options).add(hidden_options);

    const auto *executable = argv[0];
    boost::program_options::options_description visible_options(
        boost::filesystem::path(executable).filename().string() + " <input.osrm> [options]");
    visible_options.add(generic_options).add(config_options);

    // parse command line options
    boost::program_options::variables_map option_variables;
    try
    {
        boost::program_options::store(boost::program_options::command_line_parser(argc, argv)
                                          .options(cmdline_options)
                                          .positional(positional_options)
                                          .run(),
                                      option_variables);
    }
    catch (const boost::program_options::error &e)
    {
        util::Log(logERROR) << e.what();
        return return_code::fail;
    }

    if (option_variables.count("version"))
    {
        std::cout << OSRM_VERSION << std::endl;
        return return_code::exit;
    }

    if (option_variables.count("help"))
    {
        std::cout << visible_options;
        return return_code::exit;
    }

    boost::program_options::notify(option_variables);

    if (!option_variables.count("input"))
    {
        std::cout << visible_options;
        return return_code::fail;
    }

    if (config.max_cell_sizes.empty())
    {
        util::Log(logERROR) << "The maximum cell sizes array must be non-empty";
        return return_code::fail;
    }

    if (!std::is_sorted(config.max_cell_sizes.begin(), config.max_cell_sizes.end()))
    {
        util::Log(logERROR)
            << "The maximum cell sizes array must be sorted in non-descending order.";
        return return_code::fail;
    }

    if (config.max_cell_sizes.front() < 2)
    {
        util::Log(logERROR) << "Cells on the first level must have at least 2 nodes";
        return return_code::fail;
    }

    return return_code::ok;
}

int main(int argc, char *argv[]) try
{
    util::LogPolicy::GetInstance().Unmute();
    partition::PartitionConfig partition_config;

    const auto result = parseArguments(argc, argv, partition_config);

    if (return_code::fail == result)
    {
        return EXIT_FAILURE;
    }

    if (return_code::exit == result)
    {
        return EXIT_SUCCESS;
    }

    // set the default in/output names
    partition_config.UseDefaults();

    if (1 > partition_config.requested_num_threads)
    {
        util::Log(logERROR) << "Number of threads must be 1 or larger";
        return EXIT_FAILURE;
    }

    auto check_file = [](const boost::filesystem::path &path) {
        if (!boost::filesystem::is_regular_file(path))
        {
            util::Log(logERROR) << "Input file " << path << " not found!";
            return false;
        }
        else
        {
            return true;
        }
    };

    if (!check_file(partition_config.edge_based_graph_path) ||
        !check_file(partition_config.cnbg_ebg_mapping_path) ||
        !check_file(partition_config.compressed_node_based_graph_path))
    {
        return EXIT_FAILURE;
    }

    tbb::task_scheduler_init init(partition_config.requested_num_threads);
    BOOST_ASSERT(init.is_active());
    util::Log() << "Computing recursive bisection";

    TIMER_START(bisect);
    auto exitcode = partition::Partitioner().Run(partition_config);
    TIMER_STOP(bisect);
    util::Log() << "Bisection took " << TIMER_SEC(bisect) << " seconds.";

    util::DumpMemoryStats();

    return exitcode;
}
catch (const std::bad_alloc &e)
{
    util::Log(logERROR) << "[exception] " << e.what();
    util::Log(logERROR) << "Please provide more memory or consider using a larger swapfile";
    return EXIT_FAILURE;
}
#ifdef _WIN32
catch (const std::exception &e)
{
    util::Log(logERROR) << "[exception] " << e.what() << std::endl;
    return EXIT_FAILURE;
}
#endif
