#include "data_access/adapters/cache/RedisGraphStore.hpp"

#include "events/Events.hpp"
#include "insertion/BackendEncodedUuid.hpp"
#include "logging/Log.hpp"
#include "utilities/json.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace polygraph {
    namespace {

        bool supports_cache_spec(const ExpandSpec &spec) {
            return spec.direction == Direction::Both && spec.allowed_edge_types.empty() &&
                   spec.allowed_target_node_types.empty() && !spec.return_edge_refs;
        }

        std::pair<std::string, std::string> split_endpoint(std::string endpoint) {
            constexpr std::string_view redis_scheme = "redis://";
            if (endpoint.starts_with(redis_scheme))
                endpoint.erase(0, redis_scheme.size());
            const auto slash = endpoint.find('/');
            if (slash != std::string::npos)
                endpoint.resize(slash);
            const auto colon = endpoint.rfind(':');
            if (colon == std::string::npos)
                return {endpoint, "6379"};
            return {endpoint.substr(0, colon), endpoint.substr(colon + 1)};
        }

    } // namespace

    class RedisGraphStore::Client {
    public:
        Client(std::string endpoint, const int database) : endpoint_(std::move(endpoint)), database_(database) {
            if (database_ < 0) {
                throw std::invalid_argument("Redis database index must be non-negative");
            }
        }
        ~Client() { disconnect(); }

        std::vector<std::optional<std::string>> mget(const std::vector<std::string> &keys) {
            std::vector<std::optional<std::string>> values(keys.size());
            mget_visit(keys, [&](const std::size_t index, const std::optional<std::string_view> value) {
                if (value.has_value()) {
                    values[index] = std::string(*value);
                }
            });
            return values;
        }

        template<typename Visitor>
        void mget_visit(const std::vector<std::string> &keys, Visitor &&visitor) {
            EVENT_SCOPE_INFO(total_span, "redis.mget", {"key_count", keys.size()});
            if (keys.empty()) {
                return;
            }

            std::scoped_lock lock(mutex_);
            {
                EVENT_SCOPE_INFO(connect_span, "redis.connection.ensure");
                const bool was_connected = fd_ >= 0;
                ensure_connected();
                EVENT_SCOPE_INFO_FIELD(connect_span, "reused_connection", was_connected);
            }

            std::string request;
            {
                EVENT_SCOPE_INFO(build_span, "redis.mget.request_build", {"key_count", keys.size()});
                std::size_t estimated_bytes = 32U;
                for (const auto &key: keys) {
                    estimated_bytes += key.size() + 16U;
                }
                request.reserve(estimated_bytes);
                request += "*" + std::to_string(keys.size() + 1U) + "\r\n$4\r\nMGET\r\n";
                for (const auto &key: keys) {
                    request += "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
                }
                EVENT_SCOPE_INFO_FIELD(build_span, "request_bytes", request.size());
            }

            try {
                {
                    EVENT_SCOPE_INFO(send_span, "redis.mget.socket_send", {"request_bytes", request.size()});
                    send_all(request);
                }

                IoStats io_stats{};
                {
                    EVENT_SCOPE_INFO(read_span, "redis.mget.response_read_decode", {"key_count", keys.size()});
                    const std::size_t response_count = read_array_size(io_stats);
                    if (response_count != keys.size()) {
                        throw std::runtime_error("Redis MGET returned " + std::to_string(response_count) +
                                                 " values for " + std::to_string(keys.size()) + " keys");
                    }
                    for (std::size_t i = 0; i < response_count; ++i) {
                        const auto value = read_bulk_view(io_stats);
                        if (value.has_value()) {
                            ++io_stats.hit_count;
                            io_stats.payload_bytes += value->size();
                        } else {
                            ++io_stats.miss_count;
                        }
                        visitor(i, value);
                        compact_consumed();
                    }
                    EVENT_SCOPE_INFO_FIELD(read_span, "socket_recv_calls", io_stats.recv_calls);
                    EVENT_SCOPE_INFO_FIELD(read_span, "socket_recv_bytes", io_stats.recv_bytes);
                    EVENT_SCOPE_INFO_FIELD(read_span, "socket_recv_wait_ms",
                                           static_cast<double>(io_stats.recv_wait_ns) / 1'000'000.0);
                    EVENT_SCOPE_INFO_FIELD(read_span, "payload_bytes", io_stats.payload_bytes);
                    EVENT_SCOPE_INFO_FIELD(read_span, "hits", io_stats.hit_count);
                    EVENT_SCOPE_INFO_FIELD(read_span, "misses", io_stats.miss_count);
                }

                compact_consumed(true);
                EVENT_SCOPE_INFO_FIELD(total_span, "request_bytes", request.size());
                EVENT_SCOPE_INFO_FIELD(total_span, "socket_recv_calls", io_stats.recv_calls);
                EVENT_SCOPE_INFO_FIELD(total_span, "socket_recv_bytes", io_stats.recv_bytes);
                EVENT_SCOPE_INFO_FIELD(total_span, "socket_recv_wait_ms",
                                       static_cast<double>(io_stats.recv_wait_ns) / 1'000'000.0);
                EVENT_SCOPE_INFO_FIELD(total_span, "payload_bytes", io_stats.payload_bytes);
                EVENT_SCOPE_INFO_FIELD(total_span, "hits", io_stats.hit_count);
                EVENT_SCOPE_INFO_FIELD(total_span, "misses", io_stats.miss_count);
            } catch (...) {
                disconnect();
                throw;
            }
        }

    private:
        struct IoStats {
            std::size_t recv_calls{0};
            std::size_t recv_bytes{0};
            std::uint64_t recv_wait_ns{0};
            std::size_t payload_bytes{0};
            std::size_t hit_count{0};
            std::size_t miss_count{0};
        };

        void ensure_connected() {
            if (fd_ >= 0)
                return;

            auto [host, port] = split_endpoint(endpoint_);
            EVENT_SCOPE_INFO(connect_span, "redis.connection.connect", {"host", host}, {"port", port});

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *result = nullptr;

            {
                EVENT_SCOPE_INFO(resolve_span, "redis.connection.resolve", {"host", host}, {"port", port});
                const int status = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
                if (status != 0)
                    throw std::runtime_error("Redis getaddrinfo failed: " + std::string(gai_strerror(status)));
            }

            std::size_t connect_attempts = 0;
            for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
                ++connect_attempts;
                fd_ = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
                if (fd_ >= 0 && ::connect(fd_, it->ai_addr, it->ai_addrlen) == 0)
                    break;
                if (fd_ >= 0)
                    ::close(fd_);
                fd_ = -1;
            }
            ::freeaddrinfo(result);
            EVENT_SCOPE_INFO_FIELD(connect_span, "attempts", connect_attempts);
            if (fd_ < 0)
                throw std::runtime_error("Could not connect to Redis endpoint " + endpoint_);

            if (database_ != 0) {
                const std::string database = std::to_string(database_);
                const std::string request =
                        "*2\r\n$6\r\nSELECT\r\n$" + std::to_string(database.size()) + "\r\n" + database + "\r\n";
                try {
                    send_all(request);
                    IoStats stats{};
                    const auto response = read_line(stats);
                    if (response != "+OK") {
                        throw std::runtime_error("Redis SELECT failed: " + response);
                    }
                } catch (...) {
                    disconnect();
                    throw;
                }
            }
        }

        void disconnect() noexcept {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
            buffer_.clear();
            buffer_cursor_ = 0;
        }

        void send_all(std::string_view data) const {
            while (!data.empty()) {
                const ssize_t sent = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
                if (sent <= 0)
                    throw std::runtime_error("Redis socket send failed");
                data.remove_prefix(static_cast<std::size_t>(sent));
            }
        }

        [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size() - buffer_cursor_; }

        void compact_consumed(const bool force = false) {
            if (buffer_cursor_ == 0)
                return;
            if (buffer_cursor_ == buffer_.size()) {
                buffer_.clear();
                buffer_cursor_ = 0;
                return;
            }
            constexpr std::size_t compact_threshold = 1024 * 1024;
            if (force || (buffer_cursor_ >= compact_threshold && buffer_cursor_ * 2 >= buffer_.size())) {
                buffer_.erase(0, buffer_cursor_);
                buffer_cursor_ = 0;
            }
        }

        void fill(IoStats &stats) {
            compact_consumed();
            std::array<char, 256 * 1024> chunk{};
            const auto started = std::chrono::steady_clock::now();
            const ssize_t count = ::recv(fd_, chunk.data(), chunk.size(), 0);
            stats.recv_wait_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                            .count());
            if (count <= 0)
                throw std::runtime_error("Redis socket closed while reading response");
            buffer_.append(chunk.data(), static_cast<std::size_t>(count));
            ++stats.recv_calls;
            stats.recv_bytes += static_cast<std::size_t>(count);
        }

        void ensure_buffered(const std::size_t count, IoStats &stats) {
            while (buffered_bytes() < count)
                fill(stats);
        }

        std::string read_line(IoStats &stats) {
            while (true) {
                const auto pos = buffer_.find("\r\n", buffer_cursor_);
                if (pos != std::string::npos) {
                    std::string line(buffer_.data() + buffer_cursor_, pos - buffer_cursor_);
                    buffer_cursor_ = pos + 2;
                    compact_consumed();
                    return line;
                }
                fill(stats);
            }
        }

        std::size_t read_array_size(IoStats &stats) {
            const std::string header = read_line(stats);
            if (header.empty())
                throw std::runtime_error("Empty Redis response");
            if (header[0] == '-')
                throw std::runtime_error("Redis error: " + header.substr(1));
            if (header[0] != '*')
                throw std::runtime_error("Expected Redis array response");
            const long long size = std::stoll(header.substr(1));
            if (size < 0)
                throw std::runtime_error("Invalid Redis array size");
            return static_cast<std::size_t>(size);
        }

        std::optional<std::string_view> read_bulk_view(IoStats &stats) {
            const std::string header = read_line(stats);
            if (header.empty())
                throw std::runtime_error("Empty Redis response");
            if (header[0] == '-')
                throw std::runtime_error("Redis error: " + header.substr(1));
            if (header[0] != '$')
                throw std::runtime_error("Expected Redis bulk-string response");
            const long long parsed_size = std::stoll(header.substr(1));
            if (parsed_size == -1)
                return std::nullopt;
            if (parsed_size < 0)
                throw std::runtime_error("Invalid Redis bulk-string size");

            const auto size = static_cast<std::size_t>(parsed_size);
            ensure_buffered(size + 2U, stats);
            const char *value_begin = buffer_.data() + buffer_cursor_;
            if (buffer_[buffer_cursor_ + size] != '\r' || buffer_[buffer_cursor_ + size + 1U] != '\n')
                throw std::runtime_error("Malformed Redis bulk-string terminator");
            buffer_cursor_ += size + 2U;
            return std::string_view(value_begin, size);
        }

        std::string endpoint_;
        int database_{0};
        int fd_ = -1;
        std::string buffer_;
        std::size_t buffer_cursor_{0};
        std::mutex mutex_;
    };

    RedisGraphStore::RedisGraphStore(BackendDescriptor descriptor, std::string endpoint, const int database,
                                     std::string key_prefix, std::string expected_cache_plan) :
        descriptor_(std::move(descriptor)), key_prefix_(std::move(key_prefix)),
        expected_cache_plan_(std::move(expected_cache_plan)),
        client_(std::make_unique<Client>(std::move(endpoint), database)) {}
    RedisGraphStore::~RedisGraphStore() = default;

    bool RedisGraphStore::cache_ready() const {
        std::scoped_lock lock(readiness_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (readiness_.has_value() && now - readiness_checked_at_ < std::chrono::seconds(5)) {
            return *readiness_;
        }
        bool ready = false;
        try {
            auto values = client_->mget({key_prefix_ + "__manifest"});
            ready = !values.empty() && values.front().has_value();
            if (ready && !expected_cache_plan_.empty()) {
                const auto manifest = nlohmann::json::parse(*values.front(), nullptr, false);
                ready = !manifest.is_discarded() && manifest.value("cachePlan", std::string{}) == expected_cache_plan_;
                if (!ready) {
                    LOG_WARN("Redis contains a different cache plan; falling back to authoritative backends");
                }
            }
            if (!ready) {
                LOG_WARN("Redis cache is configured but not materialized; falling back to authoritative backends");
            }
        } catch (const std::exception &error) {
            LOG_WARN("Redis cache readiness check failed ({}); falling back to authoritative backends", error.what());
            ready = false;
        }
        readiness_ = ready;
        readiness_checked_at_ = now;
        return ready;
    }

    bool RedisGraphStore::can_expand(const NodeRef &, const ExpandSpec &spec) const {
        return descriptor_.capabilities.supports_batch_neighbors && descriptor_.coverage.fresh &&
               supports_cache_spec(spec) && cache_ready();
    }

    std::vector<MinimalAdjacencyRow> RedisGraphStore::get_minimal_neighbors_batch(std::span<const NodeRef> frontier,
                                                                                  const ExpandSpec &spec, int depth,
                                                                                  const BfsOptions &options) {
        (void) options;
        EVENT_SCOPE_INFO(total_span, "redis.get_minimal_neighbors", {"depth", depth},
                         {"frontier_size", frontier.size()});

        if (!supports_cache_spec(spec)) {
            throw std::runtime_error("Redis cache supports unfiltered Direction::Both minimal expansion only");
        }

        std::vector<std::string> keys;
        {
            EVENT_SCOPE_INFO(key_span, "redis.keys.build", {"frontier_size", frontier.size()});
            keys.reserve(frontier.size());
            for (const auto &source: frontier) {
                keys.push_back(key_prefix_ +
                               std::to_string(insertion::BackendEncodedId::canonical_id(source.global_id)));
            }
            std::size_t key_bytes = 0;
            for (const auto &key: keys)
                key_bytes += key.size();
            EVENT_SCOPE_INFO_FIELD(key_span, "key_bytes", key_bytes);
        }

        std::vector<MinimalAdjacencyRow> rows;
        rows.reserve(frontier.size());
        for (const auto &source: frontier) {
            rows.push_back(MinimalAdjacencyRow{.source = source, .neighbors = {}});
        }

        std::size_t total_neighbors = 0;
        std::size_t adjacency_payload_bytes = 0;
        std::size_t max_degree = 0;
        {
            EVENT_SCOPE_INFO(decode_span, "redis.adjacency.decode", {"row_count", frontier.size()});
            client_->mget_visit(keys, [&](const std::size_t i, const std::optional<std::string_view> value) {
                if (!value.has_value()) {
                    {
                        std::scoped_lock lock(readiness_mutex_);
                        readiness_ = false;
                        readiness_checked_at_ = std::chrono::steady_clock::now();
                    }
                    throw std::runtime_error(
                            "Redis cache plan is materialized incompletely: a selected source key is absent. "
                            "Reload the selected cache plan into Redis; future layers will fall back after this "
                            "failure.");
                }
                const std::string_view blob = *value;
                if (blob.size() % sizeof(std::uint64_t) != 0)
                    throw std::runtime_error("Corrupt Redis adjacency blob");
                const std::size_t count = blob.size() / sizeof(std::uint64_t);
                adjacency_payload_bytes += blob.size();
                total_neighbors += count;
                max_degree = std::max(max_degree, count);
                auto &neighbors = rows[i].neighbors;
                neighbors.reserve(count);
                for (std::size_t n = 0; n < count; ++n) {
                    std::uint64_t id = 0;
                    std::memcpy(&id, blob.data() + n * sizeof(id), sizeof(id));
                    neighbors.push_back(NodeRef{.global_id = id, .type = unresolved_type_id});
                }
            });
            EVENT_SCOPE_INFO_FIELD(decode_span, "neighbors", total_neighbors);
            EVENT_SCOPE_INFO_FIELD(decode_span, "adjacency_payload_bytes", adjacency_payload_bytes);
            EVENT_SCOPE_INFO_FIELD(decode_span, "max_degree", max_degree);
        }

        EVENT_SCOPE_INFO_FIELD(total_span, "result_rows", rows.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "neighbors", total_neighbors);
        EVENT_SCOPE_INFO_FIELD(total_span, "adjacency_payload_bytes", adjacency_payload_bytes);
        EVENT_SCOPE_INFO_FIELD(total_span, "max_degree", max_degree);
        return rows;
    }

    void RedisGraphStore::stream_neighbor_candidates(std::span<const NodeRef> frontier, const ExpandSpec &spec,
                                                     int depth, const BfsOptions &options,
                                                     const NeighborBatchConsumer &consumer) {
        EVENT_SCOPE_INFO(total_span, "redis.stream_neighbor_candidates", {"depth", depth},
                         {"frontier_size", frontier.size()}, {"mget_key_batch_size", options.redis_pipeline_batch_size},
                         {"candidate_batch_size", options.streaming_candidate_batch_size});

        if (!consumer || frontier.empty()) {
            return;
        }
        if (!supports_cache_spec(spec)) {
            throw std::runtime_error("Redis cache supports unfiltered Direction::Both minimal expansion only");
        }
        if (options.redis_pipeline_batch_size == 0 || options.streaming_candidate_batch_size == 0) {
            throw std::invalid_argument("Redis streaming batch sizes must be greater than zero");
        }

        std::vector<NodeRef> emitted;
        emitted.reserve(options.streaming_candidate_batch_size);
        std::size_t mget_count = 0;
        std::size_t total_neighbors = 0;
        std::size_t adjacency_payload_bytes = 0;
        std::size_t max_degree = 0;

        const auto flush = [&]() {
            if (!emitted.empty()) {
                consumer(std::span<const NodeRef>(emitted));
                emitted.clear();
            }
        };

        for (std::size_t offset = 0; offset < frontier.size(); offset += options.redis_pipeline_batch_size) {
            const std::size_t count = std::min(options.redis_pipeline_batch_size, frontier.size() - offset);
            EVENT_SCOPE_INFO(batch_span, "redis.stream.mget_batch", {"batch_index", mget_count},
                             {"source_count", count});

            std::vector<std::string> keys;
            keys.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const NodeRef &source = frontier[offset + i];
                keys.push_back(key_prefix_ +
                               std::to_string(insertion::BackendEncodedId::canonical_id(source.global_id)));
            }

            client_->mget_visit(keys, [&](const std::size_t, const std::optional<std::string_view> value) {
                if (!value.has_value()) {
                    {
                        std::scoped_lock lock(readiness_mutex_);
                        readiness_ = false;
                        readiness_checked_at_ = std::chrono::steady_clock::now();
                    }
                    throw std::runtime_error(
                            "Redis cache plan is materialized incompletely: a selected source key is absent. "
                            "Reload the selected cache plan into Redis; future layers will fall back after this "
                            "failure.");
                }

                const std::string_view blob = *value;
                if (blob.size() % sizeof(std::uint64_t) != 0) {
                    throw std::runtime_error("Corrupt Redis adjacency blob");
                }

                const std::size_t degree = blob.size() / sizeof(std::uint64_t);
                adjacency_payload_bytes += blob.size();
                total_neighbors += degree;
                max_degree = std::max(max_degree, degree);

                for (std::size_t n = 0; n < degree; ++n) {
                    std::uint64_t id = 0;
                    std::memcpy(&id, blob.data() + n * sizeof(id), sizeof(id));
                    emitted.push_back(NodeRef{.global_id = id, .type = unresolved_type_id});
                    if (emitted.size() >= options.streaming_candidate_batch_size) {
                        flush();
                    }
                }
            });

            ++mget_count;
            EVENT_SCOPE_INFO_FIELD(batch_span, "neighbors", total_neighbors);
        }
        flush();

        EVENT_SCOPE_INFO_FIELD(total_span, "mget_count", mget_count);
        EVENT_SCOPE_INFO_FIELD(total_span, "neighbors", total_neighbors);
        EVENT_SCOPE_INFO_FIELD(total_span, "adjacency_payload_bytes", adjacency_payload_bytes);
        EVENT_SCOPE_INFO_FIELD(total_span, "max_degree", max_degree);
    }

    std::vector<AdjacencyRow> RedisGraphStore::get_neighbors_batch(std::span<const NodeRef> frontier,
                                                                   const ExpandSpec &spec, int depth) {
        EVENT_SCOPE_INFO(total_span, "redis.get_neighbors", {"depth", depth}, {"frontier_size", frontier.size()});
        BfsOptions options{};
        auto minimal = get_minimal_neighbors_batch(frontier, spec, depth, options);

        std::vector<AdjacencyRow> rows;
        std::size_t neighbor_count = 0;
        {
            EVENT_SCOPE_INFO(convert_span, "redis.minimal_to_rich", {"row_count", minimal.size()});
            rows.reserve(minimal.size());
            for (auto &source_row: minimal) {
                AdjacencyRow row{.source = source_row.source, .neighbors = {}};
                row.neighbors.reserve(source_row.neighbors.size());
                neighbor_count += source_row.neighbors.size();
                for (const auto &neighbor: source_row.neighbors) {
                    row.neighbors.push_back(NeighborRef{.vertex = neighbor, .via_edge = std::nullopt});
                }
                rows.push_back(std::move(row));
            }
            EVENT_SCOPE_INFO_FIELD(convert_span, "neighbors", neighbor_count);
        }
        EVENT_SCOPE_INFO_FIELD(total_span, "result_rows", rows.size());
        EVENT_SCOPE_INFO_FIELD(total_span, "neighbors", neighbor_count);
        return rows;
    }

} // namespace polygraph
