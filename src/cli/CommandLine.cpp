/**
 * @file src/cli/CommandLine.cpp
 * @brief p-ranav/argparse-backed command-line adapter.
 */

#include "cli/CommandLine.hpp"

#include <argparse/argparse.hpp>

#include <charconv>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace polygraph::cli {

    struct CommandLine::Impl {
        struct Option {
            std::string key;
            std::vector<std::string> aliases;
            std::optional<std::string> default_value;
            bool flag = false;
        };

        argparse::ArgumentParser parser;
        std::vector<Option> options;
        std::unordered_map<std::string, std::size_t> option_by_key;
        std::unordered_map<std::string, std::string> values;
        bool help_requested = false;

        explicit Impl(std::string name) :
            parser(std::move(name), POLYGRAPH_VERSION, argparse::default_arguments::none) {}

        argparse::Argument &add_argument(const std::vector<std::string> &aliases) {
            if (aliases.size() == 1) {
                return parser.add_argument(aliases[0]);
            }
            if (aliases.size() == 2) {
                return parser.add_argument(aliases[0], aliases[1]);
            }
            throw std::invalid_argument("CLI options must have one or two aliases");
        }

        void validate_option(const std::string &key, const std::vector<std::string> &aliases) const {
            if (key.empty() || aliases.empty()) {
                throw std::invalid_argument("CLI options require a key and at least one alias");
            }
            if (option_by_key.contains(key)) {
                throw std::invalid_argument("Duplicate CLI option key: " + key);
            }
            for (const auto &alias: aliases) {
                if (!alias.starts_with('-')) {
                    throw std::invalid_argument("CLI option alias must start with '-': " + alias);
                }
            }
        }

        const Option &option_for_key(const std::string_view key) const {
            const auto iterator = option_by_key.find(std::string(key));
            if (iterator == option_by_key.end()) {
                throw std::logic_error("Unknown registered CLI key: " + std::string(key));
            }
            return options.at(iterator->second);
        }

        static const std::string &lookup_alias(const Option &option) { return option.aliases.back(); }
    };

    CommandLine::CommandLine(std::string program_name) : impl_(std::make_unique<Impl>(std::move(program_name))) {
        add_flag_option("help", {"-h", "--help"}, "Show this help message");
    }

    CommandLine::~CommandLine() = default;
    CommandLine::CommandLine(CommandLine &&) noexcept = default;
    CommandLine &CommandLine::operator=(CommandLine &&) noexcept = default;

    void CommandLine::add_value_option(std::string key, std::vector<std::string> aliases, std::string description,
                                       std::optional<std::string> default_value, const bool required) {
        impl_->validate_option(key, aliases);
        auto &argument = impl_->add_argument(aliases).help(std::move(description));
        if (default_value) {
            argument.default_value(*default_value);
        }
        if (required) {
            argument.required();
        }

        const auto index = impl_->options.size();
        impl_->option_by_key.emplace(key, index);
        impl_->options.push_back({.key = std::move(key),
                                  .aliases = std::move(aliases),
                                  .default_value = std::move(default_value),
                                  .flag = false});
    }

    void CommandLine::add_flag_option(std::string key, std::vector<std::string> aliases, std::string description) {
        impl_->validate_option(key, aliases);
        impl_->add_argument(aliases).help(std::move(description)).flag();

        const auto index = impl_->options.size();
        impl_->option_by_key.emplace(key, index);
        impl_->options.push_back({.key = std::move(key),
                                  .aliases = std::move(aliases),
                                  .default_value = std::string{"false"},
                                  .flag = true});
    }

    void CommandLine::parse(const int argc, char *argv[]) {
        impl_->values.clear();
        impl_->help_requested = false;

        // Do not let required application arguments hide the help screen.
        for (int index = 1; index < argc; ++index) {
            const std::string_view token(argv[index]);
            if (token == "-h" || token == "--help") {
                impl_->help_requested = true;
                return;
            }
        }

        try {
            impl_->parser.parse_args(argc, argv);
        } catch (const std::exception &error) {
            throw std::invalid_argument(error.what());
        }

        for (const auto &option: impl_->options) {
            const auto &alias = Impl::lookup_alias(option);
            if (option.flag) {
                impl_->values.emplace(option.key, impl_->parser.get<bool>(alias) ? "true" : "false");
                continue;
            }

            if (option.default_value || impl_->parser.is_used(alias)) {
                impl_->values.emplace(option.key, impl_->parser.get<std::string>(alias));
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
        if (value == "true" || value == "1") {
            return true;
        }
        if (value == "false" || value == "0") {
            return false;
        }
        throw std::invalid_argument("Expected boolean for --" + std::string(key) + ": " + value);
    }

    std::string CommandLine::help_text() const {
        std::ostringstream output;
        output << impl_->parser;
        return output.str();
    }

    std::ostream &operator<<(std::ostream &output, const CommandLine &command_line) {
        return output << command_line.help_text();
    }

} // namespace polygraph::cli
