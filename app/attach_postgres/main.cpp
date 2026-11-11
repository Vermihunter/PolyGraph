#include <iostream>
#include <memory>
#include "cli/CommandLine.hpp"
#include "cli/GraphRuntimeBuilder.hpp"
#include "data_access/adapters/PostgreSQL/RelationalAttachment.hpp"

int main(int argc, char **argv) {
    using namespace polygraph;
    try {
        cli::CommandLine args("polygraph_attach_postgres");
        args.add_value_option("schema", {"--schema"}, "Logical schema JSON", std::nullopt, true);
        args.add_value_option("mapping", {"--mapping"}, "Relational mapping JSON", std::nullopt, true);
        args.add_value_option("output", {"--output-dir"}, "New immutable identity generation directory", std::nullopt,
                              true);
        args.add_value_option("previous", {"--previous"}, "Previous relational identity manifest", std::string{});
        args.add_value_option("source", {"--source-index"}, "Adopt a prepared typed ingestion index", std::string{});
        args.parse(argc, argv);
        if (args.help_requested()) {
            std::cout << args.help_text();
            return 0;
        }
        const auto schema = cli::GraphRuntimeBuilder::load_schema(args.string_value("schema"));
        const auto mapping = postgres::load_relational_mapping(args.string_value("mapping"), *schema);
        postgres::RelationalAttachmentOptions options{.output_dir = args.string_value("output")};
        if (!args.string_value("previous").empty())
            options.previous_manifest = args.string_value("previous");
        if (!args.string_value("source").empty())
            options.source_manifest = args.string_value("source");
        // Standard libpq PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSFILE environment, no credentials in artifacts.
        std::unique_ptr<PGconn, decltype(&PQfinish)> connection(
                PQconnectdb("connect_timeout=15 application_name=polygraph-attach"), PQfinish);
        if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
            throw std::runtime_error("Cannot connect to PostgreSQL; check libpq environment");
        const auto manifest = postgres::attach_relational_nodes(connection.get(), mapping, options);
        std::cout << json{{"state", "prepared"}, {"manifest", manifest.string()}}.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
