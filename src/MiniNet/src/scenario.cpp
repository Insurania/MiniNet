#include "mininet/scenario.hpp"

#include "mininet/ack_tracker.hpp"
#include "mininet/packet.hpp"
#include "mininet/reliable_message.hpp"
#include "mininet/snapshot.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <unordered_set>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace mininet {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kScenarioSessionId = 1;

UdpEndpoint client_endpoint()
{
    return UdpEndpoint{"scenario-client", 10000};
}

UdpEndpoint server_endpoint()
{
    return UdpEndpoint{"scenario-server", 20000};
}

std::string endpoint_to_string(const UdpEndpoint& endpoint)
{
    return endpoint.address + ":" + std::to_string(endpoint.port);
}

std::vector<std::uint8_t> make_header_only_packet(PacketType type,
                                                  std::uint32_t sequence,
                                                  std::uint32_t session_id,
                                                  std::uint32_t ack,
                                                  std::uint32_t ack_bits)
{
    PacketHeader header;
    header.magic = kPacketMagic;
    header.version = kProtocolVersion;
    header.type = type;
    header.sequence = sequence;
    header.session_id = session_id;
    header.ack = ack;
    header.ack_bits = ack_bits;
    const auto encoded = encode_packet_header(header);
    return {encoded.begin(), encoded.end()};
}

std::string packet_key(const UdpEndpoint& from, const UdpEndpoint& to, const PacketHeader& header)
{
    return endpoint_to_string(from) + ">" + endpoint_to_string(to) + ":" + to_string(header.type) + ":" +
           std::to_string(header.sequence);
}

std::string json_escape(const std::string& value)
{
    std::string escaped;
    for (const auto ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

std::string json_string(const std::string& value)
{
    return "\"" + json_escape(value) + "\"";
}

std::string fixed_number(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

std::string optional_u32(bool available, std::uint32_t value)
{
    return available ? std::to_string(value) : json_string("not_available");
}

std::string optional_double(bool available, double value)
{
    return available ? fixed_number(value) : json_string("not_available");
}

std::string csv_optional_u32(bool available, std::uint32_t value)
{
    return available ? std::to_string(value) : "not_available";
}

std::string csv_optional_double(bool available, double value)
{
    return available ? fixed_number(value) : "not_available";
}

std::vector<std::uint8_t> reliable_payload(std::uint32_t index)
{
    const auto text = std::string("message-") + std::to_string(index);
    return {text.begin(), text.end()};
}

Snapshot make_scenario_snapshot(std::uint32_t snapshot_id, std::uint32_t entity_count)
{
    Snapshot snapshot;
    snapshot.snapshot_id = snapshot_id;
    snapshot.server_tick = snapshot_id;
    snapshot.server_time_ms = static_cast<ServerTimeMs>(snapshot_id) * 50;
    for (std::uint32_t index = 0; index < entity_count; ++index) {
        const auto base = static_cast<float>(snapshot_id * 10 + index);
        EntityState entity;
        entity.entity_id = 1000 + index;
        entity.position = Vec2f{base, base + 0.5f};
        entity.velocity = Vec2f{1.0f + static_cast<float>(index), -1.0f};
        snapshot.entities.push_back(entity);
    }
    return snapshot;
}

struct PacketAttempt {
    std::string key;
    UdpEndpoint from;
    UdpEndpoint to;
    PacketType type = PacketType::Heartbeat;
    std::uint32_t sequence = 0;
    std::uint32_t sent_time_ms = 0;
    bool delivered = false;
};

struct ScenarioContext {
    ScenarioResult result;
    std::vector<PacketAttempt> attempts;
    double latency_sum_ms = 0.0;
    std::uint32_t latency_samples = 0;
    std::map<std::uint32_t, std::uint32_t> reliable_send_count_by_id;
    std::unordered_set<std::uint32_t> delivered_reliable_ids;
};

std::uint32_t elapsed_ms(Clock::time_point start, Clock::time_point now)
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

void add_event(ScenarioContext& context,
               std::uint32_t time_ms,
               const std::string& type,
               const UdpEndpoint& source,
               const UdpEndpoint& target,
               std::optional<std::uint32_t> sequence = std::nullopt,
               std::optional<std::uint32_t> message_id = std::nullopt,
               std::optional<std::uint32_t> snapshot_id = std::nullopt,
               std::optional<double> latency_ms = std::nullopt,
               const std::string& details = {})
{
    ScenarioEvent event;
    event.time_ms = time_ms;
    event.event_type = type;
    event.source = endpoint_to_string(source);
    event.target = endpoint_to_string(target);
    event.packet_sequence = sequence;
    event.message_id = message_id;
    event.snapshot_id = snapshot_id;
    event.latency_ms = latency_ms;
    event.details = details;
    context.result.events.push_back(event);
}

void send_packet(ScenarioContext& context,
                 NetworkSimulator& simulator,
                 const UdpEndpoint& from,
                 const UdpEndpoint& to,
                 const std::vector<std::uint8_t>& bytes,
                 Clock::time_point start,
                 Clock::time_point now)
{
    const auto header = decode_packet_header(bytes);
    if (!header.has_value()) {
        return;
    }

    simulator.send(from, to, bytes, now);

    PacketAttempt attempt;
    attempt.key = packet_key(from, to, *header);
    attempt.from = from;
    attempt.to = to;
    attempt.type = header->type;
    attempt.sequence = header->sequence;
    attempt.sent_time_ms = elapsed_ms(start, now);
    context.attempts.push_back(attempt);
    ++context.result.stats.sent_packets;

    add_event(context, attempt.sent_time_ms, "packet_sent", from, to, header->sequence);
}

PacketAttempt* find_attempt(ScenarioContext& context, const QueuedPacket& packet, const PacketHeader& header)
{
    const auto key = packet_key(packet.from, packet.to, header);
    const auto found = std::find_if(context.attempts.begin(), context.attempts.end(), [&](const PacketAttempt& attempt) {
        return attempt.key == key;
    });
    if (found == context.attempts.end()) {
        return nullptr;
    }
    return &*found;
}

void record_delivered_packet(ScenarioContext& context,
                             const QueuedPacket& packet,
                             const PacketHeader& header,
                             Clock::time_point start)
{
    const auto time_ms = elapsed_ms(start, packet.delivery_time);
    auto* attempt = find_attempt(context, packet, header);
    const auto latency = attempt == nullptr ? 0.0 : static_cast<double>(time_ms - attempt->sent_time_ms);

    context.latency_sum_ms += latency;
    ++context.latency_samples;

    if (attempt != nullptr && !attempt->delivered) {
        attempt->delivered = true;
        ++context.result.stats.delivered_packets;
        add_event(context, time_ms, "packet_delivered", packet.from, packet.to, header.sequence, std::nullopt,
                  std::nullopt, latency);
    } else {
        ++context.result.stats.duplicate_packets;
        add_event(context, time_ms, "packet_duplicated", packet.from, packet.to, header.sequence, std::nullopt,
                  std::nullopt, latency);
    }
}

void process_server_packet(ScenarioContext& context,
                           NetworkSimulator& simulator,
                           AckTracker& client_ack,
                           AckTracker& server_ack,
                           ReliableReceiver& reliable_receiver,
                           ReliableSender& reliable_sender,
                           const QueuedPacket& packet,
                           Clock::time_point start)
{
    const auto validation = validate_packet_type(packet.bytes, PacketType::Heartbeat);
    if (!validation.accepted && validation.reason != PacketRejectReason::UnexpectedType) {
        return;
    }
    if (!validation.header.has_value()) {
        return;
    }

    const auto header = *validation.header;
    record_delivered_packet(context, packet, header, start);

    if (header.type != PacketType::ReliableData || header.session_id != kScenarioSessionId) {
        return;
    }

    server_ack.record_received(header.sequence);
    server_ack.process_ack(header.ack, header.ack_bits);

    const auto payload_size = packet.bytes.size() - kPacketHeaderSize;
    const auto decoded = decode_reliable_data_payload(ByteView(packet.bytes.data() + kPacketHeaderSize, payload_size));
    if (decoded.ok) {
        for (const auto& message : decoded.messages) {
            const auto receive_result = reliable_receiver.receive(message);
            if (receive_result.should_process) {
                context.delivered_reliable_ids.insert(receive_result.message_id);
                context.result.stats.delivered_reliable_messages =
                    static_cast<std::uint32_t>(context.delivered_reliable_ids.size());
                add_event(context, elapsed_ms(start, packet.delivery_time), "reliable_message_delivered", packet.from,
                          packet.to, header.sequence, receive_result.message_id);
            }
        }
    }

    const auto ack_sequence = server_ack.allocate_send_sequence();
    const auto ack_state = server_ack.make_ack_state();
    const auto ack_packet =
        make_header_only_packet(PacketType::Heartbeat, ack_sequence, kScenarioSessionId, ack_state.ack, ack_state.ack_bits);
    send_packet(context, simulator, server_endpoint(), client_endpoint(), ack_packet, start, packet.delivery_time);
    server_ack.record_sent(ack_sequence, packet.delivery_time);

    reliable_sender.process_acked_packets(client_ack.sent_packets());
}

void process_client_packet(ScenarioContext& context,
                           AckTracker& client_ack,
                           SnapshotBuffer& snapshot_buffer,
                           ReliableSender& reliable_sender,
                           const QueuedPacket& packet,
                           Clock::time_point start)
{
    const auto validation = validate_packet_type(packet.bytes, PacketType::Heartbeat);
    if (!validation.accepted && validation.reason != PacketRejectReason::UnexpectedType) {
        return;
    }
    if (!validation.header.has_value()) {
        return;
    }

    const auto header = *validation.header;
    record_delivered_packet(context, packet, header, start);

    if (header.session_id != kScenarioSessionId) {
        return;
    }

    client_ack.record_received(header.sequence);
    client_ack.process_ack(header.ack, header.ack_bits);
    reliable_sender.process_acked_packets(client_ack.sent_packets());

    if (header.type != PacketType::Snapshot) {
        return;
    }

    const auto payload_size = packet.bytes.size() - kPacketHeaderSize;
    const auto decoded = decode_snapshot_payload(ByteView(packet.bytes.data() + kPacketHeaderSize, payload_size));
    if (!decoded.ok) {
        return;
    }

    const auto status = snapshot_buffer.insert(decoded.snapshot);
    if (status == SnapshotInsertStatus::Inserted || status == SnapshotInsertStatus::InsertedAndEvictedOldest) {
        ++context.result.stats.delivered_snapshots;
        add_event(context, elapsed_ms(start, packet.delivery_time), "snapshot_delivered", packet.from, packet.to,
                  header.sequence, std::nullopt, decoded.snapshot.snapshot_id);
        add_event(context, elapsed_ms(start, packet.delivery_time), "snapshot_buffer_updated", packet.from, packet.to,
                  header.sequence, std::nullopt, decoded.snapshot.snapshot_id, std::nullopt,
                  "size=" + std::to_string(snapshot_buffer.size()));
    } else if (status == SnapshotInsertStatus::Duplicate) {
        ++context.result.stats.duplicate_snapshots;
    }
    context.result.stats.snapshot_buffer_size = static_cast<std::uint32_t>(snapshot_buffer.size());
}

} // namespace

ScenarioConfigValidation validate_scenario_config(const ScenarioConfig& config)
{
    if (config.loss_rate < 0.0 || config.loss_rate > 1.0) {
        return {false, "loss_rate must be in [0, 1]"};
    }
    if (config.duplicate_rate < 0.0 || config.duplicate_rate > 1.0) {
        return {false, "duplicate_rate must be in [0, 1]"};
    }
    if (config.min_latency_ms > config.max_latency_ms) {
        return {false, "min_latency_ms must be <= max_latency_ms"};
    }
    if (config.tick_ms == 0) {
        return {false, "tick_ms must be > 0"};
    }
    if (config.entities > kMaxEntitiesPerSnapshot) {
        return {false, "entities must be <= 32"};
    }

    NetworkSimulatorConfig simulator_config;
    simulator_config.loss_rate = config.loss_rate;
    simulator_config.duplicate_rate = config.duplicate_rate;
    simulator_config.min_latency = std::chrono::milliseconds(config.min_latency_ms);
    simulator_config.max_latency = std::chrono::milliseconds(config.max_latency_ms);
    simulator_config.random_seed = config.seed;
    const auto simulator_validation = validate_network_simulator_config(simulator_config);
    if (!simulator_validation.accepted) {
        return {false, "network simulator config is invalid"};
    }

    return {true, {}};
}

ScenarioResult ScenarioRunner::run(const ScenarioConfig& config) const
{
    ScenarioContext context;
    context.result.config = config;
    context.result.stats.simulation_duration_ms = config.duration_ms;

    const auto validation = validate_scenario_config(config);
    if (!validation.accepted) {
        context.result.stats.result = "FAIL";
        return context.result;
    }

    NetworkSimulatorConfig simulator_config;
    simulator_config.loss_rate = config.loss_rate;
    simulator_config.duplicate_rate = config.duplicate_rate;
    simulator_config.min_latency = std::chrono::milliseconds(config.min_latency_ms);
    simulator_config.max_latency = std::chrono::milliseconds(config.max_latency_ms);
    simulator_config.random_seed = config.seed;
    auto simulator = NetworkSimulator::create(simulator_config);
    if (!simulator.has_value()) {
        context.result.stats.result = "FAIL";
        return context.result;
    }

    AckTracker client_ack;
    AckTracker server_ack;
    ReliableSender reliable_sender;
    ReliableReceiver reliable_receiver;
    SnapshotBuffer snapshot_buffer;

    const auto start = Clock::time_point{};
    for (std::uint32_t index = 1; index <= config.messages; ++index) {
        if (reliable_sender.enqueue(reliable_payload(index), start)) {
            ++context.result.stats.sent_reliable_messages;
            add_event(context, 0, "reliable_message_sent", client_endpoint(), server_endpoint(), std::nullopt, index);
        }
    }

    std::uint32_t next_snapshot_id = 1;
    const auto tick = std::chrono::milliseconds(config.tick_ms);
    const auto duration = std::chrono::milliseconds(config.duration_ms);
    const auto drain_duration = std::chrono::milliseconds(config.max_latency_ms * 3 + config.tick_ms);
    const auto end_time = start + duration + drain_duration;

    for (auto now = start; now <= end_time; now += tick) {
        const auto within_send_window = now - start <= duration;

        if (within_send_window) {
            reliable_sender.process_acked_packets(client_ack.sent_packets());
            const auto messages = reliable_sender.select_messages_for_packet(now, kMaxDatagramSize - kPacketHeaderSize);
            if (!messages.empty()) {
                const auto sequence = client_ack.allocate_send_sequence();
                const auto ack_state = client_ack.make_ack_state();
                const auto packet =
                    make_reliable_data_packet(sequence, kScenarioSessionId, ack_state.ack, ack_state.ack_bits, messages);
                send_packet(context, *simulator, client_endpoint(), server_endpoint(), packet, start, now);
                client_ack.record_sent(sequence, now);
                reliable_sender.mark_packet_sent(sequence, messages, now);
                for (const auto& message : messages) {
                    const auto previous = context.reliable_send_count_by_id[message.message_id]++;
                    if (previous > 0) {
                        ++context.result.stats.retransmission_count;
                        add_event(context, elapsed_ms(start, now), "reliable_message_retransmitted", client_endpoint(),
                                  server_endpoint(), sequence, message.message_id);
                    }
                }
            }

            if (next_snapshot_id <= config.snapshots) {
                const auto snapshot = make_scenario_snapshot(next_snapshot_id, config.entities);
                const auto sequence = server_ack.allocate_send_sequence();
                const auto ack_state = server_ack.make_ack_state();
                const auto packet =
                    make_snapshot_packet(sequence, kScenarioSessionId, ack_state.ack, ack_state.ack_bits, snapshot);
                if (packet.has_value()) {
                    send_packet(context, *simulator, server_endpoint(), client_endpoint(), *packet, start, now);
                    server_ack.record_sent(sequence, now);
                    ++context.result.stats.sent_snapshots;
                    add_event(context, elapsed_ms(start, now), "snapshot_sent", server_endpoint(), client_endpoint(),
                              sequence, std::nullopt, snapshot.snapshot_id);
                }
                ++next_snapshot_id;
            }
        }

        const auto server_packets = simulator->poll(server_endpoint(), now);
        for (const auto& packet : server_packets) {
            process_server_packet(context, *simulator, client_ack, server_ack, reliable_receiver, reliable_sender, packet,
                                  start);
        }

        const auto client_packets = simulator->poll(client_endpoint(), now);
        for (const auto& packet : client_packets) {
            process_client_packet(context, client_ack, snapshot_buffer, reliable_sender, packet, start);
        }
    }

    reliable_sender.process_acked_packets(client_ack.sent_packets());
    context.result.stats.pending_reliable_messages = static_cast<std::uint32_t>(reliable_sender.pending_count());

    for (const auto& attempt : context.attempts) {
        if (!attempt.delivered) {
            ++context.result.stats.dropped_packets;
            add_event(context, config.duration_ms, "packet_dropped", attempt.from, attempt.to, attempt.sequence);
        }
    }

    if (context.latency_samples != 0) {
        context.result.stats.average_latency_available = true;
        context.result.stats.average_latency_ms = context.latency_sum_ms / static_cast<double>(context.latency_samples);
    }

    context.result.stats.snapshot_buffer_size = static_cast<std::uint32_t>(snapshot_buffer.size());
    return context.result;
}

std::string scenario_summary_to_json(const ScenarioResult& result)
{
    const auto& config = result.config;
    const auto& stats = result.stats;

    std::ostringstream out;
    out << "{\n";
    out << "  \"config\": {\n";
    out << "    \"loss_rate\": " << config.loss_rate << ",\n";
    out << "    \"duplicate_rate\": " << config.duplicate_rate << ",\n";
    out << "    \"min_latency_ms\": " << config.min_latency_ms << ",\n";
    out << "    \"max_latency_ms\": " << config.max_latency_ms << ",\n";
    out << "    \"seed\": " << config.seed << ",\n";
    out << "    \"messages\": " << config.messages << ",\n";
    out << "    \"snapshots\": " << config.snapshots << ",\n";
    out << "    \"entities\": " << config.entities << ",\n";
    out << "    \"duration_ms\": " << config.duration_ms << ",\n";
    out << "    \"tick_ms\": " << config.tick_ms << "\n";
    out << "  },\n";
    out << "  \"stats\": {\n";
    out << "    \"sent_packets\": " << stats.sent_packets << ",\n";
    out << "    \"delivered_packets\": " << stats.delivered_packets << ",\n";
    out << "    \"dropped_packets\": " << stats.dropped_packets << ",\n";
    out << "    \"duplicate_packets\": " << stats.duplicate_packets << ",\n";
    out << "    \"sent_reliable_messages\": " << stats.sent_reliable_messages << ",\n";
    out << "    \"delivered_reliable_messages\": " << stats.delivered_reliable_messages << ",\n";
    out << "    \"pending_reliable_messages\": " << stats.pending_reliable_messages << ",\n";
    out << "    \"retransmission_count\": " << stats.retransmission_count << ",\n";
    out << "    \"failed_reliable_messages\": "
        << optional_u32(stats.failed_reliable_messages_available, stats.failed_reliable_messages) << ",\n";
    out << "    \"sent_snapshots\": " << stats.sent_snapshots << ",\n";
    out << "    \"delivered_snapshots\": " << stats.delivered_snapshots << ",\n";
    out << "    \"duplicate_snapshots\": " << stats.duplicate_snapshots << ",\n";
    out << "    \"dropped_or_missing_snapshots\": "
        << optional_u32(stats.dropped_or_missing_snapshots_available, stats.dropped_or_missing_snapshots) << ",\n";
    out << "    \"out_of_order_snapshots\": "
        << optional_u32(stats.out_of_order_snapshots_available, stats.out_of_order_snapshots) << ",\n";
    out << "    \"snapshot_buffer_size\": " << stats.snapshot_buffer_size << ",\n";
    out << "    \"average_latency_ms\": "
        << optional_double(stats.average_latency_available, stats.average_latency_ms) << ",\n";
    out << "    \"simulation_duration_ms\": " << stats.simulation_duration_ms << ",\n";
    out << "    \"result\": " << json_string(stats.result) << "\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

std::string scenario_summary_to_csv_header()
{
    return "seed,loss_rate,duplicate_rate,min_latency_ms,max_latency_ms,sent_packets,delivered_packets,"
           "dropped_packets,duplicate_packets,sent_reliable_messages,delivered_reliable_messages,"
           "pending_reliable_messages,retransmission_count,sent_snapshots,delivered_snapshots,"
           "duplicate_snapshots,snapshot_buffer_size,average_latency_ms,result\n";
}

std::string scenario_summary_to_csv_row(const ScenarioResult& result)
{
    const auto& config = result.config;
    const auto& stats = result.stats;
    std::ostringstream out;
    out << config.seed << ',' << config.loss_rate << ',' << config.duplicate_rate << ',' << config.min_latency_ms << ','
        << config.max_latency_ms << ',' << stats.sent_packets << ',' << stats.delivered_packets << ','
        << stats.dropped_packets << ',' << stats.duplicate_packets << ',' << stats.sent_reliable_messages << ','
        << stats.delivered_reliable_messages << ',' << stats.pending_reliable_messages << ','
        << stats.retransmission_count << ',' << stats.sent_snapshots << ',' << stats.delivered_snapshots << ','
        << stats.duplicate_snapshots << ',' << stats.snapshot_buffer_size << ','
        << csv_optional_double(stats.average_latency_available, stats.average_latency_ms) << ',' << stats.result << '\n';
    return out.str();
}

std::string scenario_events_to_json(const ScenarioResult& result)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"config\": {\n";
    out << "    \"seed\": " << result.config.seed << ",\n";
    out << "    \"loss_rate\": " << result.config.loss_rate << ",\n";
    out << "    \"duplicate_rate\": " << result.config.duplicate_rate << "\n";
    out << "  },\n";
    out << "  \"stats\": {\n";
    out << "    \"sent_packets\": " << result.stats.sent_packets << ",\n";
    out << "    \"delivered_packets\": " << result.stats.delivered_packets << ",\n";
    out << "    \"dropped_packets\": " << result.stats.dropped_packets << ",\n";
    out << "    \"duplicate_packets\": " << result.stats.duplicate_packets << ",\n";
    out << "    \"result\": " << json_string(result.stats.result) << "\n";
    out << "  },\n";
    out << "  \"events\": [\n";
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        const auto& event = result.events[index];
        out << "    {\"time_ms\": " << event.time_ms << ", \"event_type\": " << json_string(event.event_type)
            << ", \"source\": " << json_string(event.source) << ", \"target\": " << json_string(event.target);
        if (event.packet_sequence.has_value()) {
            out << ", \"packet_sequence\": " << *event.packet_sequence;
        }
        if (event.message_id.has_value()) {
            out << ", \"message_id\": " << *event.message_id;
        }
        if (event.snapshot_id.has_value()) {
            out << ", \"snapshot_id\": " << *event.snapshot_id;
        }
        if (event.latency_ms.has_value()) {
            out << ", \"latency_ms\": " << fixed_number(*event.latency_ms);
        }
        if (!event.details.empty()) {
            out << ", \"details\": " << json_string(event.details);
        }
        out << "}";
        if (index + 1 != result.events.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

std::string scenario_summary_to_text(const ScenarioResult& result)
{
    const auto& stats = result.stats;
    std::ostringstream out;
    out << "MiniNet simulation summary\n";
    out << "result=" << stats.result << '\n';
    out << "sent_packets=" << stats.sent_packets << " delivered_packets=" << stats.delivered_packets
        << " dropped_packets=" << stats.dropped_packets << " duplicate_packets=" << stats.duplicate_packets << '\n';
    out << "sent_reliable_messages=" << stats.sent_reliable_messages
        << " delivered_reliable_messages=" << stats.delivered_reliable_messages
        << " pending_reliable_messages=" << stats.pending_reliable_messages
        << " retransmission_count=" << stats.retransmission_count << '\n';
    out << "sent_snapshots=" << stats.sent_snapshots << " delivered_snapshots=" << stats.delivered_snapshots
        << " duplicate_snapshots=" << stats.duplicate_snapshots
        << " snapshot_buffer_size=" << stats.snapshot_buffer_size << '\n';
    out << "average_latency_ms="
        << csv_optional_double(stats.average_latency_available, stats.average_latency_ms) << '\n';
    return out.str();
}

bool write_text_file(const std::string& path, const std::string& text, std::string* error)
{
    auto make_directory = [](const std::string& directory) {
#if defined(_WIN32)
        return _mkdir(directory.c_str());
#else
        return mkdir(directory.c_str(), 0755);
#endif
    };

    auto directory_exists = [](const std::string& directory) {
        struct stat info {
        };
        return stat(directory.c_str(), &info) == 0 && (info.st_mode & S_IFDIR) != 0;
    };

    std::string current;
    const auto last_separator = path.find_last_of("/\\");
    if (last_separator != std::string::npos) {
        const auto directory = path.substr(0, last_separator);
        for (std::size_t index = 0; index < directory.size(); ++index) {
            const auto ch = directory[index];
            current.push_back(ch);
            if (ch != '/' && ch != '\\' && index + 1 != directory.size()) {
                continue;
            }
            if (current.empty() || current == "." || current == "/" || current == "\\") {
                continue;
            }
            if (current.size() == 2 && current[1] == ':') {
                continue;
            }
            if (!directory_exists(current) && make_directory(current) != 0 && errno != EEXIST) {
                if (error != nullptr) {
                    *error = "failed to create output directory: " + current;
                }
                return false;
            }
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) {
            *error = "failed to open output file";
        }
        return false;
    }
    file << text;
    if (!file) {
        if (error != nullptr) {
            *error = "failed to write output file";
        }
        return false;
    }
    return true;
}

} // namespace mininet
