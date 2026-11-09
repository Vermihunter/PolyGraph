#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "utilities/json.hpp"

namespace polygraph {
    // Shared by native preparation and runtime source-index lookup; no filesystem writes.
    inline std::filesystem::path graph_workspace_root(const json &document, const std::filesystem::path &manifest) {
        const auto settings = document.value("workspace", json::object());
        if (!settings.is_object())
            throw std::invalid_argument("workspace must be an object");
        for (const auto &[key, value]: settings.items())
            if (key != "root" && key != "id")
                throw std::invalid_argument("workspace accepts only root and id");
        const auto name = settings.value("id", document.value("id", manifest.parent_path().filename().string()));
        const auto safe = [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        };
        if (name.empty() || name.size() > 128 || !safe(name.front()) ||
            !std::ranges::all_of(name, [&](unsigned char c) { return safe(c) || c == '.' || c == '_' || c == '-'; }))
            throw std::invalid_argument("workspace id must be a safe path component");
        const auto root = settings.value("root", std::string("artifacts"));
        if (root.empty() || root.find('\0') != std::string::npos)
            throw std::invalid_argument("workspace root must be a nonempty path without NUL");
        const std::filesystem::path base(root);
        return std::filesystem::weakly_canonical((base.is_absolute() ? base : manifest.parent_path() / base) / name);
    }

    inline std::filesystem::path workspace_reservation_path(const std::filesystem::path &root) {
        return root.parent_path() / ("." + root.filename().string() + ".polygraph-workspace.json");
    }
    inline json workspace_owner(const std::filesystem::path &manifest, const std::filesystem::path &graph) {
        return {{"format", "polygraph.artifact-owner"},
                {"version", 1},
                {"source", std::filesystem::weakly_canonical(manifest).string()},
                {"graph", std::filesystem::weakly_canonical(graph).string()}};
    }
} // namespace polygraph
