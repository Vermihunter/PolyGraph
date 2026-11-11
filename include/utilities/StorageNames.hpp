#pragma once

#include <stdexcept>
#include <string>

namespace polygraph {
    inline std::string storage_type_name(const std::string &name) {
        // Same ASCII collection/label spelling as graphio.domain.sanitize_name.
        std::string result;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const auto c = static_cast<unsigned char>(name[i]);
            if (c >= 128) {
                const std::size_t width = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
                if (width == 0 || i + width > name.size())
                    throw std::invalid_argument("Invalid UTF-8 in type name");
                for (std::size_t j = 1; j < width; ++j)
                    if ((static_cast<unsigned char>(name[i + j]) & 0xC0) != 0x80)
                        throw std::invalid_argument("Invalid UTF-8 in type name");
                result += '_';
                i += width - 1;
            } else
                result += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
                                  ? static_cast<char>(c)
                                  : '_';
        }
        if (result.empty())
            result = "X";
        if (result.front() >= '0' && result.front() <= '9')
            result = "X_" + result;
        return result;
    }
} // namespace polygraph
