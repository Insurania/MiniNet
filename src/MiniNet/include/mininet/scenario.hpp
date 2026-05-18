#pragma once

#include "mininet/network_simulator.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mininet {

struct ScenarioConfig {
    double loss_rate = 0.0;
    double duplicate_rate = 0.0;
    std::uint32_t min_latency_ms = 0;
    std::uint32_t max_latency_ms = 0;
    std::uint32_t seed = 1;

    std::uint32_t messages = 10;
    std::uint32_t snapshots = 20;
    std::uint32_t entities = 5;

    std::uint32_t duration_ms = 5000;
    std::uint32_t tick_ms = 16;

    std::string output_json_path;
    std::string output_csv_path;
    std::string event_log_path;
};

struct ScenarioConfigValidation {
    bool accepted = false;
    std::string error;
};

struct ScenarioStats {
    std::uint32_t sent_packets = 0;
    std::uint32_t delivered_packets = 0;
    std::uint32_t dropped_packets = 0;
    std::uint32_t duplicate_packets = 0;
    bool average_latency_available = false;
    double average_latency_ms = 0.0;

    std::uint32_t sent_reliable_messages = 0;
    std::uint32_t delivered_reliable_messages = 0;
    std::uint32_t pending_reliable_messages = 0;
    std::uint32_t retransmission_count = 0;
    bool failed_reliable_messages_available = false;
    std::uint32_t failed_reliable_messages = 0;

    std::uint32_t sent_snapshots = 0;
    std::uint32_t delivered_snapshots = 0;
    std::uint32_t duplicate_snapshots = 0;
    std::uint32_t snapshot_buffer_size = 0;
    bool dropped_or_missing_snapshots_available = false;
    std::uint32_t dropped_or_missing_snapshots = 0;
    bool out_of_order_snapshots_available = false;
    std::uint32_t out_of_order_snapshots = 0;

    std::uint32_t simulation_duration_ms = 0;
    std::string result = "PASS";
};

struct ScenarioEvent {
    std::uint32_t time_ms = 0;
    std::string event_type;
    std::string source;
    std::string target;
    std::optional<std::uint32_t> packet_sequence;
    std::optional<std::uint32_t> message_id;
    std::optional<std::uint32_t> snapshot_id;
    std::optional<double> latency_ms;
    std::string details;
};

struct ScenarioResult {
    ScenarioConfig config;
    ScenarioStats stats;
    std::vector<ScenarioEvent> events;
};

class ScenarioRunner {
public:
    ScenarioResult run(const ScenarioConfig& config) const;
};

ScenarioConfigValidation validate_scenario_config(const ScenarioConfig& config);

std::string scenario_summary_to_json(const ScenarioResult& result);
std::string scenario_summary_to_csv_header();
std::string scenario_summary_to_csv_row(const ScenarioResult& result);
std::string scenario_events_to_json(const ScenarioResult& result);
std::string scenario_summary_to_text(const ScenarioResult& result);

bool write_text_file(const std::string& path, const std::string& text, std::string* error);

} // namespace mininet
