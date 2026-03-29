//
// Created by akos on 19. 3. 2026.
//

#ifndef CPGDB_BFSOPTIONS_HPP
#define CPGDB_BFSOPTIONS_HPP

#include <optional>

struct BfsOptions {
    std::optional<std::size_t> maxDepth;
};

#endif //CPGDB_BFSOPTIONS_HPP