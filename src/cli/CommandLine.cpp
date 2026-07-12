/**
 * @file src/cli/CommandLine.cpp
 * @brief Implementation for command line in the command-line and runtime configuration module.
 */

#include "cli/CommandLine.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cpgdb::cli {

    struct CommandLine::Impl {
        struct Option {
            std::string key;
            std::vector<std::string> aliases;
            std::string description;
            std::optional<std::string> default_value;
            bool required = false;
            bool flag = false;
        };

        std::string program_name;
        std::vector<Option> options;
        std::unordered_map<std::string, std::size_t> option_by_alias;
        std::unordered_map<std::string, std::string> values;
        bool help_requested = false;

        explicit Impl(std::string name) : program_name(std::move(name)) {}

        void add_option(Option option) {
            if (option.key.empty() || option.aliases.empty()) {
                throw std::invalid_argument("CLI options require a key and at least one alias");
            }
            if (std::ranges::any_of(options, [&](const Option &existing) { return existing.key == option.key; })) {
                throw std::invalid_argument("Duplicate CLI option key: " + option.key);
            }

            const auto index = options.size();
            for (const auto &alias: option.aliases) {
                if (!alias.starts_with('-')) {
                    throw std::invalid_argument("CLI option alias must start with '-': " + alias);
                }
                if (!option_by_alias.emplace(alias, index).second) {
                    throw std::invalid_argument("Duplicate CLI option alias: " + alias);
                }
            }
            options.push_back(std::move(option));
        }

        const Option &option_for_key(const std::string_view key) const {
            const auto iterator = std::ranges::find(options, key, &Option::key);
            if (iterator == options.end()) {
                throw std::logic_error("Unknown registered CLI key: " + std::string(key));
            }
            return *iterator;
        }
    };

    CommandLine::CommandLine(std::string program_name) : impl_(std::make_unique<Impl>(std::move(program_name))) {
        add_flag_option("help", {"-h", "--help"}, "Show this help message");
    }

    CommandLine::~CommandLine() = default;
    CommandLine::CommandLine(CommandLine &&) noexcept = default;
    CommandLine &CommandLine::operator=(CommandLine &&) noexcept = default;

    void CommandLine::add_value_option(std::string key, std::vector<std::string> aliases, std::string description,
                                       std::optional<std::string> default_value, const bool required) {
        impl_->add_option({.key = std::move(key),
                           .aliases = std::move(aliases),
                           .description = std::move(description),
                           .default_value = std::move(default_value),
                           .required = required,
                           .flag = false});
    }

    void CommandLine::add_flag_option(std::string key, std::vector<std::string> aliases, std::string description) {
        impl_->add_option({.key = std::move(key),
                           .aliases = std::move(aliases),
                           .description = std::move(description),
                           .default_value = std::string{"false"},
                           .required = false,
                           .flag = true});
    }

    void CommandLine::parse(const int argc, char *argv[]) {
        impl_->values.clear();
        impl_->help_requested = false;

        for (const auto &option: impl_->options) {
            if (option.default_value) {
                impl_->values.emplace(option.key, *option.default_value);
            }
        }

        for (int index = 1; index < argc; ++index) {
            std::string token(argv[index]);
            std::optional<std::string> inline_value;
            if (const auto equals = token.find('='); equals != std::string::npos) {
                inline_value = token.substr(equals + 1);
                token.resize(equals);
            }

            const auto alias = impl_->option_by_alias.find(token);
            if (alias == impl_->option_by_alias.end()) {
                throw std::invalid_argument("Unknown command-line option: " + token);
            }

            const auto &option = impl_->options[alias->second];
            if (option.flag) {
                if (inline_value) {
                    throw std::invalid_argument("Flag does not take a value: " + token);
                }
                impl_->values[option.key] = "true";
                if (option.key == "help")
                    impl_->help_requested = true;
                continue;
            }

            std::string value;
            if (inline_value) {
                value = std::move(*inline_value);
            } else {
                if (index + 1 >= argc) {
                    throw std::invalid_argument("Missing value for command-line option: " + token);
                }
                value = argv[++index];
                if (impl_->option_by_alias.contains(value)) {
                    throw std::invalid_argument("Missing value for command-line option: " + token);
                }
            }
            impl_->values[option.key] = std::move(value);
        }

        if (impl_->help_requested)
            return;

        for (const auto &option: impl_->options) {
            if (option.required && !impl_->values.contains(option.key)) {
                throw std::invalid_argument("Missing required option: " + option.aliases.back());
            }
        }
    }

    bool CommandLine::help_requested() const noexcept { return impl_->help_requested; }

    const std::string &CommandLine::string_value(const std::string_view key) const {
        impl_->option_for_key(key);
        const auto iterator = impl_->values.find(std::string(key));
        if (iterator == impl_->values.end()) {
            throw std::logic_error("No value available for CLI option: " + std::string(key));
        }
        return iterator->second;
    }

    int CommandLine::int_value(const std::string_view key) const {
        const auto &value = string_value(key);
        int parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size()) {
            throw std::invalid_argument("Expected integer for --" + std::string(key) + ": " + value);
        }
        return parsed;
    }

    bool CommandLine::bool_value(const std::string_view key) const {
        const auto &value = string_value(key);
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        throw std::invalid_argument("Expected boolean for --" + std::string(key) + ": " + value);
    }

    std::string CommandLine::help_text() const {
        std::ostringstream output;
        output << "Usage: " << impl_->program_name << " [options]\n\nOptions:\n";

        std::size_t alias_width = 0;
        std::vector<std::string> aliases;
        aliases.reserve(impl_->options.size());
        for (const auto &option: impl_->options) {
            std::ostringstream joined;
            for (std::size_t i = 0; i < option.aliases.size(); ++i) {
                if (i != 0)
                    joined << ", ";
                joined << option.aliases[i];
            }
            if (!option.flag)
                joined << " <value>";
            aliases.push_back(joined.str());
            alias_width = std::max(alias_width, aliases.back().size());
        }

        for (std::size_t i = 0; i < impl_->options.size(); ++i) {
            const auto &option = impl_->options[i];
            output << "  " << std::left << std::setw(static_cast<int>(alias_width + 2)) << aliases[i]
                   << option.description;
            if (option.required) {
                output << " (required)";
            } else if (option.default_value && !option.flag && !option.default_value->empty()) {
                output << " [default: " << *option.default_value << ']';
            }
            output << '\n';
        }
        return output.str();
    }

    std::ostream &operator<<(std::ostream &output, const CommandLine &command_line) {
        return output << command_line.help_text();
    }

} // namespace cpgdb::cli
