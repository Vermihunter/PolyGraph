#include "MovieAdapter.hpp"
#include "ingestion/BulkImportCli.hpp"

int main(int argc, char **argv) {
    auto registry = polygraph::ingestion::default_input_adapters();
    registry.add(polygraph::examples::movie_graph::input_adapter());
    return polygraph::ingestion::run_bulk_import_cli(argc, argv, registry);
}
