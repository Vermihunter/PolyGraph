#ifndef POLYGRAPH_INGESTION_PROCESS_HPP
#define POLYGRAPH_INGESTION_PROCESS_HPP
#include <filesystem>
#include <string>
#include <vector>
namespace polygraph::ingestion {
    struct ProcessResult {
        int exit_code = -1;
        double seconds = 0;
        std::string stdout_text;
        std::string stderr_text;
    };
    // POSIX argv invocation; bounded captured tails, complete output in log files. No shell expansion.
    [[nodiscard]] ProcessResult run_process(const std::vector<std::string> &argv,
                                            const std::filesystem::path &log_prefix);
} // namespace polygraph::ingestion
#endif
