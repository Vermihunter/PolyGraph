#include "cli/CommandLine.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace polygraph::cli {
    namespace {

        class Arguments {
        public:
            explicit Arguments(std::vector<std::string> values) : storage_(std::move(values)) {
                argv_.reserve(storage_.size());
                for (std::string &value: storage_) {
                    argv_.push_back(value.data());
                }
            }

            [[nodiscard]] int argc() const { return static_cast<int>(argv_.size()); }

            [[nodiscard]] char **argv() { return argv_.data(); }

        private:
            std::vector<std::string> storage_;
            std::vector<char *> argv_;
        };

        class SignedIntegerOptionTest : public testing::TestWithParam<int> {};

        TEST_P(SignedIntegerOptionTest, ParsesSignedValues) {
            const int expected = GetParam();
            CommandLine command_line("command-line-test");
            command_line.add_value_option("number", {"-n", "--number"}, "Signed number", {}, true);
            Arguments arguments({"command-line-test", "--number", std::to_string(expected)});

            command_line.parse(arguments.argc(), arguments.argv());

            EXPECT_EQ(command_line.int_value("number"), expected);
        }

        INSTANTIATE_TEST_SUITE_P(SignedValues, SignedIntegerOptionTest, testing::Values(-100, -5, 0, 7, 100));

        TEST(CommandLineTest, ParsesAliasesFlagsAndDefaults) {
            CommandLine command_line("command-line-test");
            command_line.add_value_option("output", {"-o", "--output"}, "Output", "default.json");
            command_line.add_flag_option("verbose", {"-v", "--verbose"}, "Verbose mode");
            Arguments arguments({"command-line-test", "-v"});

            command_line.parse(arguments.argc(), arguments.argv());

            EXPECT_TRUE(command_line.bool_value("verbose"));
            EXPECT_EQ(command_line.string_value("output"), "default.json");
        }

        TEST(CommandLineTest, RejectsMissingRequiredOption) {
            CommandLine command_line("command-line-test");
            command_line.add_value_option("required", {"--required"}, "Required", {}, true);
            Arguments arguments({"command-line-test"});

            EXPECT_THROW(command_line.parse(arguments.argc(), arguments.argv()), std::invalid_argument);
        }

        TEST(CommandLineTest, HelpBypassesRequiredValidation) {
            CommandLine command_line("command-line-test");
            command_line.add_value_option("required", {"--required"}, "Required", {}, true);
            Arguments arguments({"command-line-test", "--help"});

            EXPECT_NO_THROW(command_line.parse(arguments.argc(), arguments.argv()));
            EXPECT_TRUE(command_line.help_requested());
        }

        TEST(CommandLineTest, RejectsOptionFollowedByAnotherOptionInsteadOfValue) {
            CommandLine command_line("command-line-test");
            command_line.add_value_option("first", {"--first"}, "First");
            command_line.add_value_option("second", {"--second"}, "Second");
            Arguments arguments({"command-line-test", "--first", "--second"});

            EXPECT_THROW(command_line.parse(arguments.argc(), arguments.argv()), std::invalid_argument);
        }

        TEST(CommandLineTest, RejectsInvalidIntegerText) {
            CommandLine command_line("command-line-test");
            command_line.add_value_option("number", {"--number"}, "Number");
            Arguments arguments({"command-line-test", "--number", "12x"});
            command_line.parse(arguments.argc(), arguments.argv());

            EXPECT_THROW((void) command_line.int_value("number"), std::invalid_argument);
        }

    } // namespace
} // namespace polygraph::cli
