#include "data_access/adapters/cache/RedisGraphStore.hpp"

#include "algorithms/ExpandSpec.hpp"
#include "algorithms/bfs/BfsOptions.hpp"
#include "infrastructure/BackendDescriptor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace polygraph {
    namespace {

        class FakeRedisServer {
        public:
            FakeRedisServer() {
                listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
                if (listen_fd_ < 0) {
                    throw std::runtime_error("socket failed");
                }
                int reuse = 1;
                ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = 0;
                if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
                    throw std::runtime_error("bind failed");
                }
                if (::listen(listen_fd_, 1) != 0) {
                    throw std::runtime_error("listen failed");
                }
                socklen_t length = sizeof(address);
                if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
                    throw std::runtime_error("getsockname failed");
                }
                port_ = ntohs(address.sin_port);
                worker_ = std::thread([this] { serve(); });
            }

            ~FakeRedisServer() {
                if (listen_fd_ >= 0) {
                    ::shutdown(listen_fd_, SHUT_RDWR);
                    ::close(listen_fd_);
                }
                if (worker_.joinable()) {
                    worker_.join();
                }
            }

            [[nodiscard]] std::string endpoint() const { return "127.0.0.1:" + std::to_string(port_); }

            [[nodiscard]] std::string request() const {
                std::scoped_lock lock(mutex_);
                return request_;
            }

        private:
            static void append_u64(std::string &output, const std::uint64_t value) {
                const auto *raw = reinterpret_cast<const char *>(&value);
                output.append(raw, sizeof(value));
            }

            static void send_fragmented(const int fd, std::string_view payload) {
                constexpr std::size_t fragment = 3;
                while (!payload.empty()) {
                    const std::size_t count = std::min(fragment, payload.size());
                    const ssize_t sent = ::send(fd, payload.data(), count, MSG_NOSIGNAL);
                    if (sent <= 0) {
                        return;
                    }
                    payload.remove_prefix(static_cast<std::size_t>(sent));
                }
            }

            void serve() {
                sockaddr_in peer{};
                socklen_t peer_length = sizeof(peer);
                const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &peer_length);
                if (fd < 0) {
                    return;
                }

                std::string request;
                char buffer[4096];
                const auto has_two_keys = [&]() {
                    const auto first = request.find("polygraph:adj:");
                    return first != std::string::npos &&
                           request.find("polygraph:adj:", first + 1U) != std::string::npos;
                };
                while (request.find("$4\r\nMGET\r\n") == std::string::npos || !has_two_keys()) {
                    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
                    if (count <= 0) {
                        ::close(fd);
                        return;
                    }
                    request.append(buffer, static_cast<std::size_t>(count));
                }
                {
                    std::scoped_lock lock(mutex_);
                    request_ = request;
                }

                std::string first;
                append_u64(first, 11);
                append_u64(first, 12);
                std::string second;
                append_u64(second, 21);

                std::string response = "*2\r\n$" + std::to_string(first.size()) + "\r\n";
                response += first;
                response += "\r\n$" + std::to_string(second.size()) + "\r\n";
                response += second;
                response += "\r\n";
                send_fragmented(fd, response);
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
            }

            int listen_fd_{-1};
            std::uint16_t port_{0};
            mutable std::mutex mutex_;
            std::string request_;
            std::thread worker_;
        };

        BackendDescriptor redis_descriptor() {
            return BackendDescriptor{
                    .id = 10,
                    .kind = BackendKind::RedisCache,
                    .capabilities = {.supports_batch_neighbors = true},
                    .coverage =
                            {
                                    .fresh = true,
                                    .covers_all_node_types = true,
                                    .covers_all_edge_types = true,
                                    .complete_for_requested_subgraph = true,
                                    .graph_version = 1,
                            },
                    .priority = 20,
                    .name = "redis-test",
            };
        }

        TEST(RedisGraphStoreTest, UsesOneMgetAndParsesFragmentedArrayResponse) {
            FakeRedisServer server;
            RedisGraphStore store(redis_descriptor(), server.endpoint(), 0, "polygraph:adj:");

            constexpr GlobalId neo4j_owner = GlobalId{1} << 48U;
            const std::vector<NodeRef> frontier{
                    NodeRef{.global_id = neo4j_owner | 1U, .type = 1},
                    NodeRef{.global_id = neo4j_owner | 2U, .type = 1},
            };
            ExpandSpec spec{};
            spec.direction = Direction::Both;
            BfsOptions options{};

            const auto rows = store.get_minimal_neighbors_batch(frontier, spec, 0, options);

            ASSERT_EQ(rows.size(), 2U);
            ASSERT_EQ(rows[0].neighbors.size(), 2U);
            EXPECT_EQ(rows[0].neighbors[0].global_id, 11U);
            EXPECT_EQ(rows[0].neighbors[1].global_id, 12U);
            ASSERT_EQ(rows[1].neighbors.size(), 1U);
            EXPECT_EQ(rows[1].neighbors[0].global_id, 21U);

            const std::string request = server.request();
            EXPECT_NE(request.find("$4\r\nMGET\r\n"), std::string::npos);
            EXPECT_EQ(request.find("$3\r\nGET\r\n"), std::string::npos);
        }

    } // namespace
} // namespace polygraph
