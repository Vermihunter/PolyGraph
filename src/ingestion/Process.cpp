#include "ingestion/Process.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "Internal.hpp"
extern char **environ;

namespace polygraph::ingestion {
    namespace {
        class FileDescriptor {
        public:
            explicit FileDescriptor(int value) : value_(value) {
                if (value_ < 0)
                    throw Error("Cannot open subprocess log: " + std::string(std::strerror(errno)));
            }
            ~FileDescriptor() { ::close(value_); }
            FileDescriptor(const FileDescriptor &) = delete;
            FileDescriptor &operator=(const FileDescriptor &) = delete;
            int get() const { return value_; }

        private:
            int value_;
        };
        std::string tail(const fs::path &path) {
            std::ifstream in(path, std::ios::binary);
            const auto size = fs::file_size(path);
            if (size > 65536)
                in.seekg(static_cast<std::streamoff>(size - 65536));
            return {std::istreambuf_iterator<char>(in), {}};
        }
    } // namespace
    ProcessResult run_process(const std::vector<std::string> &argv, const fs::path &prefix) {
        if (argv.empty() || argv.front().empty())
            throw Error("Executor command must be a nonempty argv array");
        for (const auto &arg: argv)
            if (arg.find('\0') != std::string::npos)
                throw Error("NUL in executor argument");
        fs::create_directories(prefix.parent_path());
        const auto out = prefix.string() + ".stdout.log", err = prefix.string() + ".stderr.log";
        FileDescriptor stdout_fd(::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        FileDescriptor stderr_fd(::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        FileDescriptor input_fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        struct Actions {
            posix_spawn_file_actions_t value{};
            Actions() {
                if (posix_spawn_file_actions_init(&value) != 0)
                    throw Error("posix_spawn initialization failed");
            }
            ~Actions() { posix_spawn_file_actions_destroy(&value); }
        } actions;
        if (posix_spawn_file_actions_adddup2(&actions.value, stdout_fd.get(), STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions.value, stderr_fd.get(), STDERR_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions.value, input_fd.get(), STDIN_FILENO) != 0)
            throw Error("Cannot configure subprocess descriptors");
        std::vector<char *> args;
        for (const auto &arg: argv)
            args.push_back(const_cast<char *>(arg.c_str()));
        args.push_back(nullptr);
        const auto start = detail::Clock::now();
        pid_t child = 0;
        const auto error = posix_spawnp(&child, args.front(), &actions.value, nullptr, args.data(), environ);
        if (error != 0) {
            const auto message = std::string(std::strerror(error));
            std::size_t written = 0;
            while (written < message.size()) {
                const auto bytes = ::write(stderr_fd.get(), message.data() + written, message.size() - written);
                if (bytes <= 0)
                    break;
                written += static_cast<std::size_t>(bytes);
            }
            return {.exit_code = 127,
                    .seconds = detail::elapsed(start),
                    .stdout_text = {},
                    .stderr_text = std::string(std::strerror(error))};
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0)
            if (errno != EINTR)
                throw Error("waitpid failed");
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 126;
        return {.exit_code = code,
                .seconds = detail::elapsed(start),
                .stdout_text = tail(out),
                .stderr_text = tail(err)};
    }
} // namespace polygraph::ingestion
