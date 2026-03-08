#ifndef DATABASE_TYPES_HPP
#define DATABASE_TYPES_HPP

enum class DatabaseTypes {
    JANUS,
    ARANGO,
    NEO4J,
    CLOUD_NEO4J,
    CUGRAPH,

    MIXED, // Multiple databases 
    COUNT
};

#endif // DATABASE_TYPES_HPP