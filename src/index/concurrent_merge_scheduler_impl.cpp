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

module;

// Nothing is included here on purpose: `import std;` below provides the
// threading primitives, and a TU that both includes `<memory>` and imports `std`
// trips over `shared_ptr`'s friend declarations.

module lucene_core:concurrent_merge_scheduler.impl;

import std;

import :concurrent_merge_scheduler;

import :error;
import :merge_policy;
import :merge_scheduler;
import :merge_trigger;

import std;

namespace lucene {

/// State shared between the scheduler and its workers.
///
/// It lives in a `shared_ptr` so a worker that outlives the `close()` call of a
/// scheduler that was dropped early still has valid storage to touch.
struct ConcurrentMergeScheduler::State {
    mutable std::mutex mutex;
    std::condition_variable idle;
    std::condition_variable work;
    std::deque<OneMerge> queue;
    std::vector<std::thread> workers;
    MergeSource *source{nullptr};
    size_t running{0};
    int32_t max_thread_count{1};
    bool closing{false};
    /// The first error a worker hit; handed to the next `wait_for_merges`.
    std::optional<LuceneError> error;

    /// One worker: runs merges until the queue is empty and the scheduler is
    /// being shut down.
    void work_loop(const std::shared_ptr<State> &self) {
        for (;;) {
            std::optional<OneMerge> next;
            {
                std::unique_lock<std::mutex> lock(self->mutex);
                self->work.wait(lock, [&self] { return self->closing || !self->queue.empty(); });
                if (self->queue.empty()) {
                    return;  // closing and nothing left
                }
                next = std::move(self->queue.front());
                self->queue.pop_front();
                ++self->running;
            }

            VoidResult merged;
            {
                // The merge takes the writer lock itself, so nothing of the writer
                // is held here: this is where the I/O overlaps with indexing.
                merged = self->source->merge(std::move(next.value()));
            }

            {
                const std::lock_guard<std::mutex> lock(self->mutex);
                --self->running;
                if (!merged.has_value() && !self->error.has_value()) {
                    self->error = merged.error();
                }
                if (self->queue.empty() && self->running == 0) {
                    self->idle.notify_all();
                }
            }
        }
    }

    void join_all() {
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }
};

ConcurrentMergeScheduler::ConcurrentMergeScheduler(int32_t max_thread_count) : state_(std::make_shared<State>()) {
    state_->max_thread_count = std::max<int32_t>(1, max_thread_count);
}

std::shared_ptr<ConcurrentMergeScheduler> ConcurrentMergeScheduler::create(int32_t max_thread_count) {
    return std::shared_ptr<ConcurrentMergeScheduler>(new ConcurrentMergeScheduler(max_thread_count));
}

ConcurrentMergeScheduler::~ConcurrentMergeScheduler() {
    if (!state_) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closing = true;
        state_->work.notify_all();
    }
    state_->join_all();
}

VoidResult ConcurrentMergeScheduler::merge(MergeSource &source, MergeTrigger /*trigger*/) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->source != nullptr && state_->source != &source) {
        return err(LuceneError::illegal_state("a ConcurrentMergeScheduler cannot be shared between two writers"));
    }
    state_->source = &source;
    if (state_->closing) {
        return err(LuceneError::already_closed("the merge scheduler is closed"));
    }

    // Take everything the policy queued.  The source is the writer, which holds
    // its own lock here, so draining it is safe.
    lock.unlock();
    while (true) {
        auto next = source.get_next_merge();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (!next.value().has_value()) {
            break;
        }
        lock.lock();
        state_->queue.push_back(std::move(next.value().value()));
        lock.unlock();
    }

    lock.lock();
    // One worker per queued merge, up to the configured maximum.
    while (state_->workers.size() < static_cast<size_t>(state_->max_thread_count) && !state_->queue.empty()) {
        state_->workers.emplace_back([state = state_] { state->work_loop(state); });
    }
    state_->work.notify_all();
    return {};
}

VoidResult ConcurrentMergeScheduler::wait_for_merges() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->work.notify_all();
    state_->idle.wait(lock, [this] { return state_->queue.empty() && state_->running == 0; });
    if (state_->error.has_value()) {
        LuceneError error = std::move(state_->error.value());
        state_->error.reset();
        return std::unexpected(std::move(error));
    }
    return {};
}

VoidResult ConcurrentMergeScheduler::close() {
    VoidResult waited = wait_for_merges();
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closing = true;
        state_->work.notify_all();
    }
    state_->join_all();
    return waited;
}

int32_t ConcurrentMergeScheduler::max_thread_count() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->max_thread_count;
}

VoidResult ConcurrentMergeScheduler::set_max_thread_count(int32_t count) {
    if (count < 1) {
        return err(LuceneError::illegal_argument("max_thread_count must be at least one"));
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->max_thread_count = count;
    return {};
}

size_t ConcurrentMergeScheduler::pending_merge_count() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->queue.size() + state_->running;
}

} // namespace lucene
