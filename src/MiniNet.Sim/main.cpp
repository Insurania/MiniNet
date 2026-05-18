#include "mininet/scenario.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

using namespace mininet;

void print_usage()
{
    std::cout << "MiniNet.Sim options:\n"
              << "  --loss <float>\n"
              << "  --duplicate <float>\n"
              << "  --min-latency <ms>\n"
              << "  --max-latency <ms>\n"
              << "  --seed <int>\n"
              << "  --messages <int>\n"
              << "  --snapshots <int>\n"
              << "  --entities <int>\n"
              << "  --duration-ms <int>\n"
              << "  --tick-ms <int>\n"
              << "  --output-json <path>\n"
              << "  --output-csv <path>\n"
              << "  --event-log <path>\n";
}

bool next_value(int argc, char** argv, int& index, std::string* value, std::string* error)
{
    if (index + 1 >= argc) {
        *error = std::string("missing value for ") + argv[index];
        return false;
    }
    ++index;
    *value = argv[index];
    return true;
}

bool parse_double_value(const std::string& text, double* value)
{
    try {
        std::size_t parsed = 0;
        const auto converted = std::stod(text, &parsed);
        if (parsed != text.size()) {
            return false;
        }
        *value = converted;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_u32_value(const std::string& text, std::uint32_t* value)
{
    if (text.empty() || text[0] == '-') {
        return false;
    }
    try {
        std::size_t parsed = 0;
        const auto converted = std::stoul(text, &parsed, 10);
        if (parsed != text.size() || converted > 0xFFFFFFFFul) {
            return false;
        }
        *value = static_cast<std::uint32_t>(converted);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_args(int argc, char** argv, ScenarioConfig* config, std::string* error)
{
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (name == "--help" || name == "-h") {
            print_usage();
            std::exit(EXIT_SUCCESS);
        }

        std::string value;
        if (!next_value(argc, argv, index, &value, error)) {
            return false;
        }

        if (name == "--loss") {
            if (!parse_double_value(value, &config->loss_rate)) {
                *error = "invalid --loss value";
                return false;
            }
        } else if (name == "--duplicate") {
            if (!parse_double_value(value, &config->duplicate_rate)) {
                *error = "invalid --duplicate value";
                return false;
            }
        } else if (name == "--min-latency") {
            if (!parse_u32_value(value, &config->min_latency_ms)) {
                *error = "invalid --min-latency value";
                return false;
            }
        } else if (name == "--max-latency") {
            if (!parse_u32_value(value, &config->max_latency_ms)) {
                *error = "invalid --max-latency value";
                return false;
            }
        } else if (name == "--seed") {
            if (!parse_u32_value(value, &config->seed)) {
                *error = "invalid --seed value";
                return false;
            }
        } else if (name == "--messages") {
            if (!parse_u32_value(value, &config->messages)) {
                *error = "invalid --messages value";
                return false;
            }
        } else if (name == "--snapshots") {
            if (!parse_u32_value(value, &config->snapshots)) {
                *error = "invalid --snapshots value";
                return false;
            }
        } else if (name == "--entities") {
            if (!parse_u32_value(value, &config->entities)) {
                *error = "invalid --entities value";
                return false;
            }
        } else if (name == "--duration-ms") {
            if (!parse_u32_value(value, &config->duration_ms)) {
                *error = "invalid --duration-ms value";
                return false;
            }
        } else if (name == "--tick-ms") {
            if (!parse_u32_value(value, &config->tick_ms)) {
                *error = "invalid --tick-ms value";
                return false;
            }
        } else if (name == "--output-json") {
            config->output_json_path = value;
        } else if (name == "--output-csv") {
            config->output_csv_path = value;
        } else if (name == "--event-log") {
            config->event_log_path = value;
        } else {
            *error = "unknown argument: " + name;
            return false;
        }
    }

    return true;
}

bool write_outputs(const ScenarioResult& result, std::string* error)
{
    if (!result.config.output_json_path.empty() &&
        !write_text_file(result.config.output_json_path, scenario_summary_to_json(result), error)) {
        return false;
    }

    if (!result.config.output_csv_path.empty()) {
        const auto csv = scenario_summary_to_csv_header() + scenario_summary_to_csv_row(result);
        if (!write_text_file(result.config.output_csv_path, csv, error)) {
            return false;
        }
    }

    if (!result.config.event_log_path.empty() &&
        !write_text_file(result.config.event_log_path, scenario_events_to_json(result), error)) {
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    ScenarioConfig config;
    std::string error;
    if (!parse_args(argc, argv, &config, &error)) {
        std::cerr << "error: " << error << '\n';
        print_usage();
        return EXIT_FAILURE;
    }

    const auto validation = validate_scenario_config(config);
    if (!validation.accepted) {
        std::cerr << "error: " << validation.error << '\n';
        return EXIT_FAILURE;
    }

    const ScenarioRunner runner;
    const auto result = runner.run(config);
    std::cout << scenario_summary_to_text(result);

    if (!write_outputs(result, &error)) {
        std::cerr << "error: " << error << '\n';
        return EXIT_FAILURE;
    }

    return result.stats.result == "PASS" ? EXIT_SUCCESS : EXIT_FAILURE;
}
