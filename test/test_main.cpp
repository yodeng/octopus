// Copyright (c) 2016 Daniel Cooke
// Use of this source code is governed by the MIT license that can be found in the LICENSE file.

#include <iostream>
#include <cstdlib>
#include <chrono>
#include <exception>

#include <config/config.hpp>
#include <config/common.hpp>
#include <config/option_parser.hpp>
#include <config/option_collation.hpp>
#include <core/octopus.hpp>
#include <utils/string_utils.hpp>
#include <utils/timing.hpp>
#include <exceptions/error.hpp>
#include <logging/error_handler.hpp>
#include <logging/logging.hpp>
#include <logging/main_logging.hpp>

#include "mock_options.hpp"

using namespace octopus;
using namespace octopus::options;

namespace {

template <typename E>
auto log_exception(const E& e)
{
    log_error(e);
    
    log_program_end();
    
    return EXIT_FAILURE;
}

template <typename E>
auto log_startup_exception(const E& e)
{
    logging::init();
    
    log_program_startup();
    
    return log_exception(e);
}

void init_common(const OptionMap& options)
{
    logging::init(get_debug_log_file_name(options), get_trace_log_file_name(options));
    
    DEBUG_MODE = options::is_debug_mode(options);
    TRACE_MODE = options::is_trace_mode(options);
}

} // namespace

int main(const int argc, const char** argv)
{
    OptionMap options;
    
    try {
        options = test::get_basic_mock_options();
    } catch (const Error& e) {
        log_startup_exception(e);
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        log_startup_exception(e);
        return EXIT_FAILURE;
    } catch (...) {
        logging::init();
        
        log_program_end();
        
        return EXIT_FAILURE;
    }
    
    if (is_run_command(options)) {
        try {
            init_common(options);
            
            log_program_startup();
            
            logging::InfoLogger info_log {};
            
            const auto start = std::chrono::system_clock::now();
            
            auto components = collate_genome_calling_components(options);
            
            auto end = std::chrono::system_clock::now();
            
            using utils::TimeInterval;
            
            stream(info_log) << "Done initialising calling components in " << TimeInterval {start, end};
            
            options.clear();
            
            if (components) run_octopus(*components);
            
            log_program_end();
        } catch (const Error& e) {
            return log_exception(e);
        }
    }
    
    return EXIT_SUCCESS;
}
