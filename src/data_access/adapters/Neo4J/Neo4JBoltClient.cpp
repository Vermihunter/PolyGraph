/**
 * @file src/data_access/adapters/Neo4J/Neo4JBoltClient.cpp
 * @brief Implementation for neo4 jbolt client in the graph-store access module.
 */

#include "data_access/adapters/Neo4J/Neo4JBoltClient.hpp"

#include "utilities/json.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace polygraph {

    struct Neo4JBoltClient::Impl {
    public:
        Impl(std::string endpoint, std::string username, std::string password, std::string database_name) :
            endpoint_(std::move(endpoint)), username_(std::move(username)), password_(std::move(password)),
            database_name_(std::move(database_name)) {}

        ~Impl() { close_socket(); }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;

        json run(const std::string &cypher, const json &parameters) {
            ensure_connected();

            const json extra = database_name_.empty() ? json::object() : json{{"db", database_name_}};

            send_message(pack_struct(0x10, json::array({cypher, parameters, extra})));
            send_message(pack_struct(0x3F, json::array({json{{"n", -1}}})));

            const BoltMessage run_response = read_message_struct();
            if (run_response.signature == 0x7F) {
                throw std::runtime_error("Neo4J Bolt RUN failed: " + failure_message(run_response.fields));
            }
            if (run_response.signature != 0x70) {
                throw std::runtime_error("Neo4J Bolt expected SUCCESS after RUN, got signature 0x" +
                                         hex_byte(run_response.signature));
            }

            json data = json::array();

            while (true) {
                const BoltMessage message = read_message_struct();

                if (message.signature == 0x71) { // RECORD
                    if (message.fields.empty() || !message.fields[0].is_array()) {
                        throw std::runtime_error("Neo4J Bolt RECORD has unexpected shape: " + message.fields.dump());
                    }

                    data.push_back(json{{"row", message.fields[0]}});
                    continue;
                }

                if (message.signature == 0x70) { // SUCCESS after PULL
                    break;
                }

                if (message.signature == 0x7F) { // FAILURE
                    throw std::runtime_error("Neo4J Bolt PULL failed: " + failure_message(message.fields));
                }

                if (message.signature == 0x7E) { // IGNORED
                    throw std::runtime_error("Neo4J Bolt PULL was ignored");
                }

                throw std::runtime_error("Neo4J Bolt unexpected message signature 0x" + hex_byte(message.signature));
            }

            return json{{"results", json::array({json{{"data", data}}})}, {"errors", json::array()}};
        }

    private:
        struct BoltUri {
            std::string host;
            std::string port;
        };

        struct BoltMessage {
            std::uint8_t signature{};
            json fields = json::array();
        };

        std::string endpoint_;
        std::string username_;
        std::string password_;
        std::string database_name_;

        int socket_fd_ = -1;
        int bolt_major_ = 0;
        int bolt_minor_ = 0;

        static std::string hex_byte(std::uint8_t byte) {
            constexpr char digits[] = "0123456789ABCDEF";
            std::string out;
            out.push_back(digits[(byte >> 4) & 0x0F]);
            out.push_back(digits[byte & 0x0F]);
            return out;
        }

        static BoltUri parse_bolt_uri(const std::string &endpoint) {
            std::string uri = endpoint;

            constexpr const char *prefix = "bolt://";
            if (uri.rfind(prefix, 0) == 0) {
                uri.erase(0, std::strlen(prefix));
            }

            const auto slash = uri.find('/');
            if (slash != std::string::npos) {
                uri.resize(slash);
            }

            std::string host = uri;
            std::string port = "7687";

            const auto colon = uri.rfind(':');
            if (colon != std::string::npos) {
                host = uri.substr(0, colon);
                port = uri.substr(colon + 1);
            }

            if (host.empty()) {
                throw std::runtime_error("Invalid Bolt endpoint: " + endpoint);
            }

            return BoltUri{host, port};
        }

        void close_socket() {
            if (socket_fd_ >= 0) {
                ::close(socket_fd_);
                socket_fd_ = -1;
            }
        }

        void ensure_connected() {
            if (socket_fd_ >= 0) {
                return;
            }

            connect_socket();
            handshake();
            authenticate();
        }

        void connect_socket() {
            const BoltUri uri = parse_bolt_uri(endpoint_);

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            addrinfo *result = nullptr;
            const int rc = ::getaddrinfo(uri.host.c_str(), uri.port.c_str(), &hints, &result);
            if (rc != 0) {
                throw std::runtime_error("getaddrinfo(" + uri.host + ":" + uri.port +
                                         ") failed: " + ::gai_strerror(rc));
            }

            for (addrinfo *p = result; p != nullptr; p = p->ai_next) {
                socket_fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (socket_fd_ < 0) {
                    continue;
                }

                if (::connect(socket_fd_, p->ai_addr, p->ai_addrlen) == 0) {
                    break;
                }

                close_socket();
            }

            ::freeaddrinfo(result);

            if (socket_fd_ < 0) {
                throw std::runtime_error("Could not connect to Neo4J Bolt endpoint " + endpoint_);
            }
        }

        static void append_u32_be(std::vector<std::uint8_t> &out, std::uint32_t value) {
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        }

        static std::uint32_t read_u32_be(const std::uint8_t *bytes) {
            return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
        }

        void send_all(const std::vector<std::uint8_t> &bytes) {
            std::size_t sent = 0;
            while (sent < bytes.size()) {
                const ssize_t n = ::send(socket_fd_, bytes.data() + sent, bytes.size() - sent, 0);
                if (n <= 0) {
                    close_socket();
                    throw std::runtime_error("Bolt socket send failed: " + std::string(std::strerror(errno)));
                }
                sent += static_cast<std::size_t>(n);
            }
        }

        void recv_exact(std::uint8_t *buffer, std::size_t size) {
            std::size_t read = 0;
            while (read < size) {
                const ssize_t n = ::recv(socket_fd_, buffer + read, size - read, 0);
                if (n <= 0) {
                    close_socket();
                    throw std::runtime_error("Bolt socket recv failed/closed: " + std::string(std::strerror(errno)));
                }
                read += static_cast<std::size_t>(n);
            }
        }

        void handshake() {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(20);

            append_u32_be(bytes, 0x6060B017);

            // Offer stable common versions.  Neo4J 5.x can choose 5.1; Neo4J 4.4
            // can choose 4.4.  Newer minor features are not needed by this client.
            append_u32_be(bytes, 0x00000501);
            append_u32_be(bytes, 0x00000500);
            append_u32_be(bytes, 0x00000404);
            append_u32_be(bytes, 0x00000403);

            send_all(bytes);

            std::uint8_t response[4]{};
            recv_exact(response, 4);

            const std::uint32_t chosen = read_u32_be(response);
            if (chosen == 0) {
                throw std::runtime_error("Neo4J Bolt server rejected all offered protocol versions");
            }

            bolt_major_ = static_cast<int>((chosen >> 8) & 0xFF);
            bolt_minor_ = static_cast<int>(chosen & 0xFF);
        }

        void authenticate() {
            if (bolt_major_ == 5 && bolt_minor_ >= 1) {
                send_message(pack_struct(0x01, json::array({json{{"user_agent", "polygraph-bolt/0.1"}}})));
                expect_success("HELLO");

                send_message(pack_struct(0x6A, json::array({auth_token()})));
                expect_success("LOGON");
                return;
            }

            json hello = auth_token();
            hello["user_agent"] = "polygraph-bolt/0.1";
            send_message(pack_struct(0x01, json::array({hello})));
            expect_success("HELLO");
        }

        [[nodiscard]] json auth_token() const {
            if (username_.empty()) {
                return json{{"scheme", "none"}};
            }

            return json{{"scheme", "basic"}, {"principal", username_}, {"credentials", password_}};
        }

        void expect_success(const std::string &operation) {
            const BoltMessage message = read_message_struct();
            if (message.signature == 0x70) {
                return;
            }

            if (message.signature == 0x7F) {
                throw std::runtime_error("Neo4J Bolt " + operation + " failed: " + failure_message(message.fields));
            }

            throw std::runtime_error("Neo4J Bolt " + operation + " expected SUCCESS, got signature 0x" +
                                     hex_byte(message.signature));
        }

        static std::string failure_message(const json &fields) {
            if (!fields.is_array() || fields.empty() || !fields[0].is_object()) {
                return fields.dump();
            }

            const json &meta = fields[0];
            const std::string code = meta.value("code", "");
            const std::string message = meta.value("message", "");

            if (!code.empty() || !message.empty()) {
                return code + ": " + message;
            }

            return meta.dump();
        }

        void send_message(const std::vector<std::uint8_t> &payload) {
            std::size_t offset = 0;
            while (offset < payload.size()) {
                const std::size_t chunk = std::min<std::size_t>(payload.size() - offset, 65535);
                std::vector<std::uint8_t> framed;
                framed.reserve(2 + chunk);
                framed.push_back(static_cast<std::uint8_t>((chunk >> 8) & 0xFF));
                framed.push_back(static_cast<std::uint8_t>(chunk & 0xFF));
                framed.insert(framed.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                              payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
                send_all(framed);
                offset += chunk;
            }

            const std::vector<std::uint8_t> end_chunk{0, 0};
            send_all(end_chunk);
        }

        std::vector<std::uint8_t> read_chunked_message() {
            std::vector<std::uint8_t> payload;

            while (true) {
                std::uint8_t header[2]{};
                recv_exact(header, 2);
                const std::uint16_t size =
                        (static_cast<std::uint16_t>(header[0]) << 8) | static_cast<std::uint16_t>(header[1]);

                if (size == 0) {
                    break;
                }

                const std::size_t old_size = payload.size();
                payload.resize(old_size + size);
                recv_exact(payload.data() + old_size, size);
            }

            return payload;
        }

        BoltMessage read_message_struct() {
            const auto payload = read_chunked_message();
            if (payload.empty()) {
                throw std::runtime_error("Neo4J Bolt received empty message");
            }

            std::size_t offset = 0;
            const std::uint8_t marker = payload[offset++];
            if ((marker & 0xF0) != 0xB0) {
                throw std::runtime_error("Neo4J Bolt expected struct marker, got 0x" + hex_byte(marker));
            }

            const std::size_t field_count = marker & 0x0F;
            if (offset >= payload.size()) {
                throw std::runtime_error("Neo4J Bolt struct has no signature");
            }

            const std::uint8_t signature = payload[offset++];

            json fields = json::array();
            for (std::size_t i = 0; i < field_count; ++i) {
                fields.push_back(decode_value(payload, offset));
            }

            return BoltMessage{signature, fields};
        }

        static void append_i16(std::vector<std::uint8_t> &out, std::int16_t value) {
            out.push_back(static_cast<std::uint8_t>((static_cast<std::uint16_t>(value) >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xFF));
        }

        static void append_i32(std::vector<std::uint8_t> &out, std::int32_t value) {
            append_u32_be(out, static_cast<std::uint32_t>(value));
        }

        static void append_i64(std::vector<std::uint8_t> &out, std::int64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> shift) & 0xFF));
            }
        }

        static void pack_string(std::vector<std::uint8_t> &out, const std::string &value) {
            const std::size_t size = value.size();
            if (size <= 15) {
                out.push_back(static_cast<std::uint8_t>(0x80 | size));
            } else if (size <= 255) {
                out.push_back(0xD0);
                out.push_back(static_cast<std::uint8_t>(size));
            } else if (size <= 65535) {
                out.push_back(0xD1);
                out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
                out.push_back(static_cast<std::uint8_t>(size & 0xFF));
            } else {
                out.push_back(0xD2);
                append_u32_be(out, static_cast<std::uint32_t>(size));
            }

            out.insert(out.end(), value.begin(), value.end());
        }

        static void pack_list_header(std::vector<std::uint8_t> &out, std::size_t size) {
            if (size <= 15) {
                out.push_back(static_cast<std::uint8_t>(0x90 | size));
            } else if (size <= 255) {
                out.push_back(0xD4);
                out.push_back(static_cast<std::uint8_t>(size));
            } else if (size <= 65535) {
                out.push_back(0xD5);
                out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
                out.push_back(static_cast<std::uint8_t>(size & 0xFF));
            } else {
                out.push_back(0xD6);
                append_u32_be(out, static_cast<std::uint32_t>(size));
            }
        }

        static void pack_map_header(std::vector<std::uint8_t> &out, std::size_t size) {
            if (size <= 15) {
                out.push_back(static_cast<std::uint8_t>(0xA0 | size));
            } else if (size <= 255) {
                out.push_back(0xD8);
                out.push_back(static_cast<std::uint8_t>(size));
            } else if (size <= 65535) {
                out.push_back(0xD9);
                out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
                out.push_back(static_cast<std::uint8_t>(size & 0xFF));
            } else {
                out.push_back(0xDA);
                append_u32_be(out, static_cast<std::uint32_t>(size));
            }
        }

        static void pack_integer(std::vector<std::uint8_t> &out, std::int64_t value) {
            if (value >= -16 && value <= 127) {
                out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
            } else if (value >= INT8_MIN && value <= INT8_MAX) {
                out.push_back(0xC8);
                out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(value)));
            } else if (value >= INT16_MIN && value <= INT16_MAX) {
                out.push_back(0xC9);
                append_i16(out, static_cast<std::int16_t>(value));
            } else if (value >= INT32_MIN && value <= INT32_MAX) {
                out.push_back(0xCA);
                append_i32(out, static_cast<std::int32_t>(value));
            } else {
                out.push_back(0xCB);
                append_i64(out, value);
            }
        }

        static void pack_value(std::vector<std::uint8_t> &out, const json &value) {
            if (value.is_null()) {
                out.push_back(0xC0);
                return;
            }

            if (value.is_boolean()) {
                out.push_back(value.get<bool>() ? 0xC3 : 0xC2);
                return;
            }

            if (value.is_number_integer()) {
                pack_integer(out, value.get<std::int64_t>());
                return;
            }

            if (value.is_number_unsigned()) {
                const auto u = value.get<std::uint64_t>();
                if (u > static_cast<std::uint64_t>(INT64_MAX)) {
                    throw std::runtime_error("PackStream cannot encode uint64 larger than int64 max");
                }
                pack_integer(out, static_cast<std::int64_t>(u));
                return;
            }

            if (value.is_number_float()) {
                union {
                    double d;
                    std::uint64_t u;
                } bits{value.get<double>()};

                out.push_back(0xC1);
                for (int shift = 56; shift >= 0; shift -= 8) {
                    out.push_back(static_cast<std::uint8_t>((bits.u >> shift) & 0xFF));
                }
                return;
            }

            if (value.is_string()) {
                pack_string(out, value.get<std::string>());
                return;
            }

            if (value.is_array()) {
                pack_list_header(out, value.size());
                for (const auto &item: value) {
                    pack_value(out, item);
                }
                return;
            }

            if (value.is_object()) {
                pack_map_header(out, value.size());
                for (auto it = value.begin(); it != value.end(); ++it) {
                    pack_string(out, it.key());
                    pack_value(out, it.value());
                }
                return;
            }

            throw std::runtime_error("Unsupported JSON value for PackStream: " + value.dump());
        }

        static std::vector<std::uint8_t> pack_struct(std::uint8_t signature, const json &fields) {
            if (!fields.is_array() || fields.size() > 15) {
                throw std::runtime_error("Bolt struct fields must be an array of size <= 15");
            }

            std::vector<std::uint8_t> out;
            out.push_back(static_cast<std::uint8_t>(0xB0 | fields.size()));
            out.push_back(signature);

            for (const auto &field: fields) {
                pack_value(out, field);
            }

            return out;
        }

        static std::size_t read_size(const std::vector<std::uint8_t> &in, std::size_t &offset, std::uint8_t marker,
                                     std::uint8_t tiny_base, std::uint8_t marker8, std::uint8_t marker16,
                                     std::uint8_t marker32) {
            if ((marker & 0xF0) == tiny_base) {
                return marker & 0x0F;
            }

            if (marker == marker8) {
                require_available(in, offset, 1);
                return in[offset++];
            }

            if (marker == marker16) {
                require_available(in, offset, 2);
                const std::size_t size =
                        (static_cast<std::size_t>(in[offset]) << 8) | static_cast<std::size_t>(in[offset + 1]);
                offset += 2;
                return size;
            }

            if (marker == marker32) {
                require_available(in, offset, 4);
                const std::size_t size = read_u32_be(in.data() + offset);
                offset += 4;
                return size;
            }

            throw std::runtime_error("Unexpected PackStream size marker 0x" + hex_byte(marker));
        }

        static void require_available(const std::vector<std::uint8_t> &in, std::size_t offset, std::size_t size) {
            if (offset + size > in.size()) {
                throw std::runtime_error("Unexpected end of PackStream payload");
            }
        }

        static std::int64_t read_signed(const std::vector<std::uint8_t> &in, std::size_t &offset, std::size_t bytes) {
            require_available(in, offset, bytes);

            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bytes; ++i) {
                value = (value << 8) | in[offset++];
            }

            const int shift = static_cast<int>((8 - bytes) * 8);
            return static_cast<std::int64_t>(value << shift) >> shift;
        }

        static json decode_value(const std::vector<std::uint8_t> &in, std::size_t &offset) {
            require_available(in, offset, 1);
            const std::uint8_t marker = in[offset++];

            if (marker <= 0x7F) {
                return static_cast<std::int64_t>(marker);
            }
            if (marker >= 0xF0) {
                return static_cast<std::int64_t>(static_cast<std::int8_t>(marker));
            }

            if ((marker & 0xF0) == 0x80 || marker == 0xD0 || marker == 0xD1 || marker == 0xD2) {
                const std::size_t size = read_size(in, offset, marker, 0x80, 0xD0, 0xD1, 0xD2);
                require_available(in, offset, size);
                std::string value(reinterpret_cast<const char *>(in.data() + offset), size);
                offset += size;
                return value;
            }

            if ((marker & 0xF0) == 0x90 || marker == 0xD4 || marker == 0xD5 || marker == 0xD6) {
                const std::size_t size = read_size(in, offset, marker, 0x90, 0xD4, 0xD5, 0xD6);
                json result = json::array();
                for (std::size_t i = 0; i < size; ++i) {
                    result.push_back(decode_value(in, offset));
                }
                return result;
            }

            if ((marker & 0xF0) == 0xA0 || marker == 0xD8 || marker == 0xD9 || marker == 0xDA) {
                const std::size_t size = read_size(in, offset, marker, 0xA0, 0xD8, 0xD9, 0xDA);
                json result = json::object();
                for (std::size_t i = 0; i < size; ++i) {
                    json key = decode_value(in, offset);
                    if (!key.is_string()) {
                        throw std::runtime_error("PackStream map key is not a string");
                    }
                    result[key.get<std::string>()] = decode_value(in, offset);
                }
                return result;
            }

            if ((marker & 0xF0) == 0xB0) {
                const std::size_t size = marker & 0x0F;
                require_available(in, offset, 1);
                const std::uint8_t sig = in[offset++];
                json fields = json::array();
                for (std::size_t i = 0; i < size; ++i) {
                    fields.push_back(decode_value(in, offset));
                }
                return json{{"__bolt_struct_signature", sig}, {"fields", fields}};
            }

            switch (marker) {
                case 0xC0:
                    return nullptr;
                case 0xC2:
                    return false;
                case 0xC3:
                    return true;
                case 0xC8:
                    return read_signed(in, offset, 1);
                case 0xC9:
                    return read_signed(in, offset, 2);
                case 0xCA:
                    return read_signed(in, offset, 4);
                case 0xCB:
                    return read_signed(in, offset, 8);
                case 0xC1: {
                    require_available(in, offset, 8);
                    std::uint64_t bits = 0;
                    for (int i = 0; i < 8; ++i) {
                        bits = (bits << 8) | in[offset++];
                    }
                    union {
                        std::uint64_t u;
                        double d;
                    } value{bits};
                    return value.d;
                }
                default:
                    throw std::runtime_error("Unsupported PackStream marker 0x" + hex_byte(marker));
            }
        }
    };

    Neo4JBoltClient::Neo4JBoltClient(std::string endpoint, std::string username, std::string password,
                                     std::string database_name) :
        impl_(std::make_unique<Impl>(std::move(endpoint), std::move(username), std::move(password),
                                     std::move(database_name))) {}

    Neo4JBoltClient::~Neo4JBoltClient() = default;

    json Neo4JBoltClient::run(const std::string &cypher, const json &parameters) {
        return impl_->run(cypher, parameters);
    }

} // namespace polygraph
