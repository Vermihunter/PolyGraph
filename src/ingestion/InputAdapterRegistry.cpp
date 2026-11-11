#include "ingestion/InputAdapterRegistry.hpp"

#include <algorithm>
#include <utility>

namespace polygraph::ingestion {
    void InputAdapterRegistry::add(InputAdapterDefinition definition) {
        if (definition.version.empty())
            throw Error("Adapter " + definition.name + " requires a nonempty version");
        add_definition(std::move(definition));
    }

    void InputAdapterRegistry::add_definition(InputAdapterDefinition definition) {
        const auto valid = [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        };
        if (definition.name.empty() || !std::ranges::all_of(definition.name, valid))
            throw Error("Adapter name must contain only lowercase ASCII letters, digits, '-', '_' or '.'");
        if (!definition.factory || !definition.input_files)
            throw Error("Adapter " + definition.name + " requires factory and input_files callbacks");
        const auto name = definition.name;
        if (!definitions_.emplace(name, std::move(definition)).second)
            throw Error("Duplicate adapter: " + name);
    }

    const InputAdapterDefinition &InputAdapterRegistry::get(std::string_view name) const {
        if (const auto it = definitions_.find(name); it != definitions_.end())
            return it->second;
        std::string available;
        for (const auto &[registered, definition]: definitions_) {
            (void) definition;
            if (!available.empty())
                available += ", ";
            available += registered;
        }
        throw Error("Unknown adapter: " + std::string(name) +
                    " (available: " + (available.empty() ? "none" : available) + ")");
    }

    std::vector<std::string> InputAdapterRegistry::names() const {
        std::vector<std::string> result;
        result.reserve(definitions_.size());
        for (const auto &[name, definition]: definitions_) {
            (void) definition;
            result.push_back(name);
        }
        return result;
    }

    std::unique_ptr<IGraphInputAdapter> InputAdapterRegistry::create(const PrepareOptions &options,
                                                                     const fs::path &scratch) const {
        auto adapter = get(options.adapter).factory(options, scratch);
        if (!adapter)
            throw Error("Adapter " + options.adapter + " factory returned null");
        return adapter;
    }
} // namespace polygraph::ingestion
