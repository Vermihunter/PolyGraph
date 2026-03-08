//
// Created by akos on 6. 3. 2026.
//

#ifndef CPGDB_SCHEDULER_HPP
#define CPGDB_SCHEDULER_HPP

#include <engine/pipeline/Task.hpp>

class Scheduler {

public:

    template<typename F>
    auto emplace(F&& func) {
        using R = std::invoke_result_t<F>;

        auto storage = std::make_shared<std::optional<R>>();

        auto node = taskflow.emplace(
            [func=std::forward<F>(func), storage]() mutable {

                if constexpr(std::is_void_v<R>)
                {
                    func();
                }
                else
                {
                    storage->emplace(func());
                }

            });

        return Task<R>(node, storage);
    }

    template<typename A, typename B>
    void precede(Task<A>& a, Task<B>& b)
    {
        a.node().precede(b.node());
    }

    template<typename F, typename... Tasks>
    auto then(F&& func, Tasks&... deps) {
        using R = std::invoke_result_t<F, decltype(std::move(deps.get()))...>;

        auto storage = std::make_shared<std::optional<R>>();

        auto node = taskflow.emplace(
            [func=std::forward<F>(func), storage, &deps...]() mutable {

                storage->emplace(
                    func(std::move(deps.get())...)
                );

            });

        (deps.node().precede(node), ...);

        return Task<R>(node, storage);
    }

    void run() {
        executor.run(taskflow).wait();
    }

private:

    tf::Executor executor;
    tf::Taskflow taskflow;
};


#endif //CPGDB_SCHEDULER_HPP