#pragma once

#include "ingestion/InputAdapterRegistry.hpp"

namespace polygraph::examples::movie_graph {
    [[nodiscard]] ingestion::InputAdapterDefinition input_adapter();
}
