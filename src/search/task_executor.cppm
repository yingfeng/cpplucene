/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

export module lucene_core:task_executor;

import :types;
import std;

import :error;

namespace lucene {

/// Runs the tasks a search is split into, one task per leaf slice.
///
/// Port of `TaskExecutor` (`core/search/task_executor.rs`).
///
/// **Deviations:** the reference implementation runs on a shared `rayon` pool
/// and cancels the remaining tasks as soon as one of them fails; this one owns
/// its threads, lets the tasks that already started finish and then reports the
/// first error in task order.  The reference implementation's offloaded-task
/// counter is test-only instrumentation and is not ported.
export class TaskExecutor final {
public:
    /// An executor that runs every task on the calling thread, in order.
    [[nodiscard]] static std::shared_ptr<TaskExecutor> direct();

    /// An executor with `threads` workers; `0` gives [`direct`].
    [[nodiscard]] static Result<std::shared_ptr<TaskExecutor>> with_threads(size_t threads);

    /// How many tasks may run at the same time.
    [[nodiscard]] size_t parallelism() const noexcept { return threads_ == 0 ? 1 : threads_; }

    /// Whether every task runs on the calling thread.
    [[nodiscard]] bool is_direct() const noexcept { return threads_ == 0; }

    /// Runs every task and returns the results in the order the tasks were
    /// given; the first failure in that order is reported.  Each task is invoked
    /// exactly once, so it may be move-only (a task owns the collector of its
    /// slice).
    ///
    /// The calling thread takes part in the work, so a search does not block one
    /// thread per task while it waits.
    ///
    /// `T` is the value type the tasks answer, and has to be given explicitly
    /// (`invoke_all<std::unique_ptr<Collector>>(std::move(tasks))`).
    template <typename T, typename Task>
    [[nodiscard]] Result<std::vector<T>> invoke_all(std::vector<Task> tasks) const {
        static_assert(std::is_invocable_r_v<Result<T>, Task &>, "a task must answer a Result<T>");
        const size_t count = tasks.size();
        std::vector<T> values;
        values.reserve(count);
        if (count == 0) {
            return values;
        }

        std::vector<std::optional<Result<T>>> results(count);
        std::atomic<size_t> next{0};
        const auto run = [&tasks, &results, &next, count]() {
            while (true) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count) {
                    return;
                }
                // A task reports its failures; a thrown exception would end the
                // process from a worker thread, so it is turned into an error.
                try {
                    results[index].emplace(tasks[index]());
                } catch (const std::exception &exception) {
                    results[index].emplace(
                            err(LuceneError::illegal_state("a search task threw: " + std::string(exception.what()))));
                } catch (...) {
                    results[index].emplace(err(LuceneError::illegal_state("a search task threw an unknown exception")));
                }
            }
        };

        if (threads_ == 0 || count == 1) {
            run();
        } else {
            const size_t workers = std::min(threads_, count - 1);
            std::vector<std::thread> threads;
            threads.reserve(workers);
            for (size_t i = 0; i < workers; ++i) {
                threads.emplace_back(run);
            }
            run();
            for (std::thread &thread : threads) {
                thread.join();
            }
        }

        for (std::optional<Result<T>> &result : results) {
            if (!result.has_value()) {
                return err(LuceneError::illegal_state("a search task produced no result"));
            }
            if (!result->has_value()) {
                return std::unexpected(std::move(result->error()));
            }
            values.push_back(std::move(result->value()));
        }
        return values;
    }

private:
    explicit TaskExecutor(size_t threads) noexcept : threads_(threads) {}

    /// `0` when the executor runs every task on the calling thread.
    size_t threads_{0};
};

} // namespace lucene
