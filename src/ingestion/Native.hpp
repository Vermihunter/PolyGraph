#ifndef POLYGRAPH_INGESTION_NATIVE_HPP
#define POLYGRAPH_INGESTION_NATIVE_HPP
#include "Internal.hpp"
namespace polygraph::ingestion::detail {
    [[nodiscard]] std::unique_ptr<IImportExecutor> native_executor(ImportContext context);
    [[nodiscard]] std::unique_ptr<IImportExecutor> postgres_relational_executor(ImportContext context);
    [[nodiscard]] Json resolved_relational_targets(Json config, const fs::path &path);
    [[nodiscard]] std::unique_ptr<IImportExecutor> postgres_executor(ImportContext context);
    [[nodiscard]] std::unique_ptr<IImportExecutor> arango_executor(ImportContext context);
    [[nodiscard]] std::unique_ptr<IImportExecutor> neo4j_executor(ImportContext context);
    [[nodiscard]] std::string environment_secret(const Json &config, const std::string &key);
    [[nodiscard]] Json http_json(const Json &target, const std::string &method, const std::string &path,
                                 const std::string &body = {}, bool allow_not_found = false);
    [[nodiscard]] Json expected_counts(const ImportContext &context);
    [[nodiscard]] bool expects_partition_metadata(const ImportContext &context);
    void verify_expected_rows(const ImportContext &context, const std::function<void(bool, const Json &)> &check);
} // namespace polygraph::ingestion::detail
#endif
