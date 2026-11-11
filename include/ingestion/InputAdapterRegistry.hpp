#ifndef POLYGRAPH_INGESTION_INPUT_ADAPTER_REGISTRY_HPP
#define POLYGRAPH_INGESTION_INPUT_ADAPTER_REGISTRY_HPP

#include "ingestion/Import.hpp"

#include <map>
#include <string_view>

namespace polygraph::ingestion {
    // Explicit compatibility policies and generic typed-source allocation.
    enum class InputIdPolicy { canonical, ldbc_block31, typed_external };

    [[nodiscard]] std::string input_id_strategy(InputIdPolicy policy);

    struct InputAdapterDefinition {
        std::string name;
        std::function<std::unique_ptr<IGraphInputAdapter>(const PrepareOptions &, const fs::path &)> factory;
        std::function<std::vector<fs::path>(const PrepareOptions &)> input_files;
        InputIdPolicy id_policy = InputIdPolicy::canonical;
        // Nonempty versions participate in input identity. Built-ins retain their legacy fingerprint.
        std::string version = "1";
        // Optional read-only inspection of leading metadata/source table names. Never use scratch or emit records.
        std::function<Json(const PrepareOptions &)> inspect = {};
    };

    // Populate at application setup, then pass as const for the duration of preparation.
    // Factories are linked C++ code; there is no global registration or dynamic loading.
    class InputAdapterRegistry {
    public:
        void add(InputAdapterDefinition definition);
        [[nodiscard]] const InputAdapterDefinition &get(std::string_view name) const;
        [[nodiscard]] std::vector<std::string> names() const;
        [[nodiscard]] std::unique_ptr<IGraphInputAdapter> create(const PrepareOptions &options,
                                                                 const fs::path &scratch) const;

    private:
        friend const InputAdapterRegistry &default_input_adapters();
        // Only the built-in composition root can retain unversioned legacy identities.
        void add_definition(InputAdapterDefinition definition);
        std::map<std::string, InputAdapterDefinition, std::less<>> definitions_;
    };

    [[nodiscard]] const InputAdapterRegistry &default_input_adapters();
} // namespace polygraph::ingestion
#endif
