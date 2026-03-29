//
// Created by Akos on 6. 3. 2026.
//

#ifndef CPGDB_TASK_HPP
#define CPGDB_TASK_HPP

#include <taskflow/taskflow.hpp>

template<typename T>
class Task {
public:
    Task(const tf::Task& node, std::shared_ptr<std::optional<T>> storage)
        : node_(node), storage_(storage) {}

    // Return by reference for inspection
    T& get() & { return **storage_; }

    // Return by rvalue reference to allow moving the unique_ptr out
    T&& get() && { return std::move(**storage_); }

    [[nodiscard]] tf::Task node() const { return node_; }

private:
    tf::Task node_;
    std::shared_ptr<std::optional<T>> storage_;
};

#endif //CPGDB_TASK_HPP