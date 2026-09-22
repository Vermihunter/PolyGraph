/**
 * @file include/cli/CommandLine.hpp
 * @brief Declarations for command line in the command-line and runtime configuration module.
 */

#pragma once

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace polygraph::cli {

class CommandLine {
  public:
    explicit CommandLine(std::string program_name);
    ~CommandLine();

    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;
    CommandLine(CommandLine&&) noexcept;
    CommandLine& operator=(CommandLine&&) noexcept;

    void add_value_option(std::string key,
                          std::vector<std::string> aliases,
                          std::string description,
                          std::optional<std::string> default_value = std::nullopt,
                          bool required = false);
    void
    add_flag_option(std::string key, std::vector<std::string> aliases, std::string description);

    void parse(int argc, char* argv[]);

    [[nodiscard]] bool help_requested() const noexcept;
    [[nodiscard]] const std::string& string_value(std::string_view key) const;
    [[nodiscard]] int int_value(std::string_view key) const;
    [[nodiscard]] bool bool_value(std::string_view key) const;
    [[nodiscard]] std::string help_text() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::ostream& operator<<(std::ostream& output, const CommandLine& command_line);

} // namespace polygraph::cli
