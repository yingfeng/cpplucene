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

module lucene_core:documents_writer.impl;

import :documents_writer;

import :codec;
import :directory;
import :documents_writer_per_thread;
import :document;
import :error;
import :index_writer_config;
import :segment_commit_info;
import :segment_info;
import :sort_field;
import :string_helper;
import :term;
import :types;
import :version;

import std;

namespace lucene {

namespace {

/// The files of a segment, so an aborted writer can remove what it wrote.
/// The number a segment name carries (`_12` -> 12); `-1` when it carries none.
///
/// Segments are published in this order, so a commit keeps the document ids in
/// indexing order; comparing the names as strings would put `_10` before `_2`.
int64_t segment_number(std::string_view name) {
    if (name.size() < 2 || name.front() != '_') {
        return -1;
    }
    int64_t number = 0;
    for (const char digit : name.substr(1)) {
        if (digit < '0' || digit > '9') {
            return -1;
        }
        number = number * 10 + (digit - '0');
    }
    return number;
}

std::set<std::string> files_of(const std::vector<SegmentCommitInfo> &segments) {
    std::set<std::string> files;
    for (const auto &segment : segments) {
        files.insert(segment.info.files.begin(), segment.info.files.end());
    }
    return files;
}

} // namespace

struct DocumentsWriter::Impl {
    Impl(Directory &directory, const IndexWriterConfig &config, int64_t next_segment_counter)
        : directory_(directory),
          codec_(config.codec()),
          analyzer_(config.analyzer()),
          similarity_(config.similarity()),
          index_created_version_major_(config.index_created_version_major()),
          max_buffered_docs_(config.max_buffered_docs()),
          ram_buffer_size_(config.ram_buffer_size_bytes()),
          flush_policy_(config.flush_policy()),
          analyzer_provider_(config.analyzer_provider()),
          index_sort_(config.index_sort()),
          next_segment_counter_(next_segment_counter) {}

    /// One indexing buffer and the thread that claimed it.
    ///
    /// **Deviation:** the reference implementation keeps a `ThreadState` per thread
    /// in a `DocumentsWriterPerThreadPool`; here a slot is the buffer plus the id of
    /// its thread, and a thread indexes into its own slot without taking the pool
    /// lock (only the slot's own lock, which nobody else contends for).
    struct Slot {
        mutable std::mutex mutex;
        std::unique_ptr<DocumentsWriterPerThread> buffer;
        /// The analyzer of this slot, when the configuration provides one.
        std::shared_ptr<Analyzer> analyzer;
        std::thread::id owner{};
    };

    /// The slots, copied so the caller can work without the registry lock.
    [[nodiscard]] std::vector<Slot *> slots_snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Slot *> slots;
        slots.reserve(slots_.size());
        for (const std::unique_ptr<Slot> &slot : slots_) {
            slots.push_back(slot.get());
        }
        return slots;
    }

    /// Bytes the buffers hold.
    [[nodiscard]] int64_t bytes_used_in_ram() const {
        int64_t total = 0;
        for (Slot *slot : slots_snapshot()) {
            const std::lock_guard<std::mutex> guard(slot->mutex);
            if (slot->buffer != nullptr) {
                total += slot->buffer->bytes_used_in_ram();
            }
        }
        return total;
    }

    /// Documents buffered but not written out yet, including the buffers a caller is
    /// writing right now.
    [[nodiscard]] int32_t pending_doc_count() const {
        int32_t total = 0;
        for (Slot *slot : slots_snapshot()) {
            const std::lock_guard<std::mutex> guard(slot->mutex);
            if (slot->buffer != nullptr) {
                total += slot->buffer->num_docs_in_ram();
            }
        }
        return total + static_cast<int32_t>(in_flight_docs_.load());
    }

    void set_max_buffers(size_t count) noexcept {
        max_buffers_ = std::max<size_t>(1, count);
        buffer_released_.notify_all();
    }

    [[nodiscard]] size_t max_buffers() const noexcept { return max_buffers_.load(); }

    [[nodiscard]] size_t stalled_writers() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stalled_writers_;
    }

    [[nodiscard]] SegmentInfo new_segment_info() {
        SegmentInfo info;
        info.name = "_" + std::to_string(next_segment_counter_.fetch_add(1));
        const auto random = StringHelper::random_id();
        info.id.assign(random.begin(), random.end());
        info.version = LUCENE_CURRENT_VERSION;
        info.min_version = LUCENE_CURRENT_VERSION;
        info.codec_name = std::string(codec_.name());
        info.diagnostics.emplace("source", "flush");
        info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());
        return info;
    }

    /// The slot of the calling thread, claiming one when it has none yet.
    ///
    /// A thread indexes into its own slot, so the analysis and the postings building
    /// of different threads run at the same time.  When there are more indexing
    /// threads than buffers, the extra threads share the last slot; only their
    /// indexing is then serialised.
    [[nodiscard]] Slot &slot_for_thread() {
        const std::thread::id id = std::this_thread::get_id();
        const std::lock_guard<std::mutex> lock(mutex_);
        if (const auto found = slot_of_thread_.find(id); found != slot_of_thread_.end()) {
            return *slots_[found->second];
        }
        if (slots_.empty()) {
            slots_.push_back(std::make_unique<Slot>());
        }
        size_t index = slots_.size() - 1;
        if (slots_.size() < max_buffers_.load()) {
            index = slots_.size();
            slots_.push_back(std::make_unique<Slot>());
        }
        slots_[index]->owner = id;
        slot_of_thread_.emplace(id, index);
        return *slots_[index];
    }

    /// One flush worker: writes the buffers other threads hand over.
    void flush_loop() {
        for (;;) {
            std::unique_ptr<DocumentsWriterPerThread> buffer;
            {
                std::unique_lock<std::mutex> lock(flush_mutex_);
                flush_work_.wait(lock, [this] { return flush_closing_ || !flush_queue_.empty(); });
                if (flush_queue_.empty()) {
                    return;  // closing and nothing left
                }
                buffer = std::move(flush_queue_.front());
                flush_queue_.pop_front();
            }
            VoidResult written;
            if (flush_sink_ != nullptr) {
                written = flush_sink_(std::move(buffer));
            } else {
                written = write_buffer(buffer.get());
            }
            if (!written.has_value()) {
                const std::lock_guard<std::mutex> lock(flush_mutex_);
                if (!flush_error_.has_value()) {
                    flush_error_ = written.error();
                }
            }
        }
    }

    /// Writes `buffer` and queues its segment, without a sink.
    [[nodiscard]] VoidResult write_buffer(DocumentsWriterPerThread *buffer) {
        const int32_t flushed_docs = buffer->num_docs_in_ram();
        auto flushed = buffer->flush();
        if (!flushed.has_value()) {
            (void)publish_flushed(std::nullopt, flushed_docs);
            return std::unexpected(std::move(flushed.error()));
        }
        return publish_flushed(std::move(flushed.value()), flushed_docs);
    }

    void start_flush_threads(DocumentsWriter::FlushSink sink, size_t count) {
        {
            const std::lock_guard<std::mutex> lock(flush_mutex_);
            flush_sink_ = std::move(sink);
            flush_closing_ = false;
        }
        for (size_t i = 0; i < count; ++i) {
            flush_workers_.emplace_back([this] { flush_loop(); });
        }
    }

    [[nodiscard]] VoidResult stop_flush_threads() {
        {
            const std::lock_guard<std::mutex> lock(flush_mutex_);
            flush_closing_ = true;
            flush_work_.notify_all();
        }
        for (std::thread &worker : flush_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        flush_workers_.clear();
        std::optional<LuceneError> error;
        {
            const std::lock_guard<std::mutex> lock(flush_mutex_);
            error = std::move(flush_error_);
            flush_error_.reset();
        }
        if (error.has_value()) {
            return std::unexpected(std::move(error.value()));
        }
        return {};
    }

    [[nodiscard]] bool enqueue_flush(std::unique_ptr<DocumentsWriterPerThread> &buffer) {
        {
            const std::lock_guard<std::mutex> lock(flush_mutex_);
            if (flush_workers_.empty() || flush_closing_) {
                return false;
            }
            flush_queue_.push_back(std::move(buffer));
        }
        flush_work_.notify_one();
        return true;
    }

    [[nodiscard]] size_t flush_thread_count() const noexcept {
        const std::lock_guard<std::mutex> lock(flush_mutex_);
        return flush_workers_.size();
    }

    /// Gives `slot` an empty buffer; the caller holds the slot's lock.
    void ensure_slot_buffer(Slot &slot) {
        if (slot.buffer != nullptr) {
            return;
        }
        Analyzer *slot_analyzer = &analyzer_;
        if (analyzer_provider_ != nullptr) {
            slot.analyzer = analyzer_provider_();
            if (slot.analyzer != nullptr) {
                slot_analyzer = slot.analyzer.get();
            }
        }
        slot.buffer = std::make_unique<DocumentsWriterPerThread>(
                directory_, codec_, new_segment_info(), index_created_version_major_, *slot_analyzer, similarity_,
                index_sort_);
    }

    /// The flush thresholds of one buffer.
    ///
    /// The RAM budget and the document count describe the whole writer, so every slot
    /// gets its share: that keeps the total bounded by the configured size instead of
    /// multiplying it by the number of slots.
    [[nodiscard]] FlushPolicyContext flush_context(const DocumentsWriterPerThread &buffer) const {
        const int64_t buffers = static_cast<int64_t>(max_buffers_.load());
        const int64_t per_buffer_bytes = std::max<int64_t>(1, ram_buffer_size_ / buffers);
        const int32_t per_buffer_docs = max_buffered_docs_ <= 0
                                                ? max_buffered_docs_
                                                : std::max(1, max_buffered_docs_ / static_cast<int32_t>(buffers));
        return FlushPolicyContext{buffer.num_docs_in_ram(), buffer.bytes_used_in_ram(), per_buffer_docs,
                                  per_buffer_bytes};
    }

    /// Writes the buffer of every slot out.
    ///
    /// The slot locks serialise this with the threads that index; a thread that keeps
    /// adding while a commit flushes leaves its documents for the next commit, exactly
    /// like a single-buffered writer does.
    [[nodiscard]] VoidResult flush_slots() {
        for (Slot *slot : slots_snapshot()) {
            std::unique_ptr<DocumentsWriterPerThread> buffer;
            {
                const std::lock_guard<std::mutex> guard(slot->mutex);
                if (slot->buffer == nullptr) {
                    continue;
                }
                if (slot->buffer->num_docs_in_ram() == 0) {
                    slot->buffer.reset();
                    continue;
                }
                buffer = std::move(slot->buffer);
            }
            auto flushed = buffer->flush();
            // The buffer is spent whether or not the flush worked: the reference
            // implementation drops the DWPT from its pool in both cases.
            if (!flushed.has_value()) {
                return std::unexpected(std::move(flushed.error()));
            }
            if (auto published = publish_flushed(std::move(flushed.value()), 0); !published.has_value()) {
                return published;
            }
        }
        return {};
    }

    /// Adds `document` to the slot's buffer; the caller holds the slot's lock.
    [[nodiscard]] VoidResult add_to_slot(Slot &slot, const Document &document) {
        if (slot.analyzer != nullptr) {
            // The slot has its own analyzer, so the analysis runs in parallel too.
            return slot.buffer->add_document(document);
        }
        // The configured analyzer is shared and not thread safe, so the analysis of
        // this document runs alone.
        const std::lock_guard<std::mutex> analysis(analysis_mutex_);
        return slot.buffer->add_document(document);
    }

    /// Buffers `document`; answers the buffer to write out when it filled up.
    [[nodiscard]] Result<std::unique_ptr<DocumentsWriterPerThread>> add_document_async(const Document &document) {
        Slot &slot = slot_for_thread();
        const std::lock_guard<std::mutex> guard(slot.mutex);
        ensure_slot_buffer(slot);
        const FlushPolicyContext context = flush_context(*slot.buffer);
        if (flush_policy_ != nullptr && flush_policy_->should_flush(context) && slot.buffer->num_docs_in_ram() > 0) {
            // The full buffer goes to the caller to be written out; this document still
            // has to be buffered, so a fresh buffer is taken for the slot.
            std::unique_ptr<DocumentsWriterPerThread> full = std::move(slot.buffer);
            buffers_in_flight_.fetch_add(1);
            in_flight_docs_.fetch_add(full->num_docs_in_ram());
            ensure_slot_buffer(slot);
            if (auto added = add_to_slot(slot, document); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            return full;
        }
        if (auto added = add_to_slot(slot, document); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        return std::unique_ptr<DocumentsWriterPerThread>{};
    }

    /// Takes a written buffer back and queues its segment.
    [[nodiscard]] VoidResult publish_flushed(std::optional<SegmentCommitInfo> segment, int32_t flushed_docs) {
        // The segment goes into the queue *before* the buffer stops counting as in
        // flight: a commit that waits for the counters to reach zero must not miss a
        // segment that is already written.
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (segment.has_value()) {
                publish_segment_locked(std::move(segment.value()));
            }
            if (buffers_in_flight_.load() > 0) {
                buffers_in_flight_.fetch_sub(1);
            }
            if (flushed_docs != 0) {
                in_flight_docs_.fetch_sub(flushed_docs);
            }
        }
        // Every waiter re-checks the predicate: a single notification would leave the
        // other threads asleep until the next flush, and that flush may only happen
        // once they return.
        buffer_released_.notify_all();
        return {};
    }

    /// Queues a written segment; the caller holds the registry lock.
    void publish_segment_locked(SegmentCommitInfo segment) {
        ++flush_count_;
        // Segments are published in *numeric* name order (`_2` before `_10`), so a
        // commit keeps the document ids in indexing order even when two threads flush
        // at the same time.
        const auto position = std::upper_bound(flushed_segments_.begin(), flushed_segments_.end(),
                                               segment_number(segment.info.name),
                                               [](int64_t number, const SegmentCommitInfo &other) {
                                                   return number < segment_number(other.info.name);
                                               });
        flushed_segments_.insert(position, std::move(segment));
    }

    /// Buffers `document`, writing the buffer out here when it filled up.
    ///
    /// This is the path a caller uses when it does not want to own the buffer:
    /// the write happens inside the call (still without the writer lock) and the
    /// document is buffered afterwards.
    [[nodiscard]] VoidResult add_document(const Document &document) {
        // `add_document_async` buffers the document itself, so this only has to write
        // out the buffer it answers (if any) — no loop.
        auto full = add_document_async(document);
        if (!full.has_value()) {
            return std::unexpected(std::move(full.error()));
        }
        if (full.value() == nullptr) {
            return {};
        }
        const int32_t flushed_docs = full.value()->num_docs_in_ram();
        auto written = full.value()->flush();
        if (!written.has_value()) {
            (void)publish_flushed(std::nullopt, flushed_docs);
            return std::unexpected(std::move(written.error()));
        }
        return publish_flushed(std::move(written.value()), flushed_docs);
    }

    [[nodiscard]] Result<std::vector<SegmentCommitInfo>> flush_all() {
        // A buffer another thread is writing has to be published before the commit
        // takes the queue, so the commit sees every document the caller added.
        if (auto waited = wait_for_in_flight_flushes(); !waited.has_value()) {
            return std::unexpected(std::move(waited.error()));
        }
        if (auto flushed = flush_slots(); !flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        return take_flushed_segments();
    }

    [[nodiscard]] VoidResult wait_for_in_flight_flushes() {
        {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            buffer_released_.wait(lock, [this] { return buffers_in_flight_.load() == 0; });
        }
        // A flush worker that failed has to surface the error on the next wait, so a
        // commit does not silently drop a segment.
        std::optional<LuceneError> error;
        {
            const std::lock_guard<std::mutex> lock(flush_mutex_);
            error = std::move(flush_error_);
            flush_error_.reset();
        }
        if (error.has_value()) {
            return std::unexpected(std::move(error.value()));
        }
        return {};
    }

    [[nodiscard]] size_t pending_flush_count() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return flushed_segments_.size();
    }

    [[nodiscard]] bool has_pending_flushes() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !flushed_segments_.empty();
    }

    [[nodiscard]] int64_t pending_flushed_doc_count() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        int64_t total = 0;
        for (const SegmentCommitInfo &segment : flushed_segments_) {
            total += segment.max_doc();
        }
        return total;
    }

    [[nodiscard]] std::vector<SegmentCommitInfo> take_flushed_segments() {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SegmentCommitInfo> segments = std::move(flushed_segments_);
        flushed_segments_.clear();
        return segments;
    }

    [[nodiscard]] VoidResult rollback() {
        if (auto waited = wait_for_in_flight_flushes(); !waited.has_value()) {
            return waited;
        }
        for (Slot *slot : slots_snapshot()) {
            const std::lock_guard<std::mutex> guard(slot->mutex);
            if (slot->buffer != nullptr) {
                (void)slot->buffer->abort();
                slot->buffer.reset();
            }
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        // Remove the files of the segments that will never be committed.
        for (const std::string &file : files_of(flushed_segments_)) {
            (void)directory_.delete_file(file);
        }
        flushed_segments_.clear();
        delete_queue_.clear();
        delete_all_ = false;
        return {};
    }

    Directory &directory_;
    const Codec &codec_;
    Analyzer &analyzer_;
    std::shared_ptr<const Similarity> similarity_;
    int32_t index_created_version_major_;
    int32_t max_buffered_docs_;
    int64_t ram_buffer_size_;
    std::shared_ptr<const FlushPolicy> flush_policy_;
    std::function<std::shared_ptr<Analyzer>()> analyzer_provider_;
    /// Serialises the analysis of the buffers that share the configured analyzer,
    /// which is not thread safe.  Unused when every buffer brings its own.
    std::mutex analysis_mutex_;
    std::optional<Sort> index_sort_;
    std::atomic<int64_t> next_segment_counter_;

    /// Guards the slot registry, the flush queue and the counters below.
    mutable std::mutex mutex_;
    /// Guards the wait for the buffers that are being written out; it is separate so
    /// a thread never holds the registry lock while waiting.
    mutable std::mutex wait_mutex_;
    /// The queue the flush workers drain, and their state.
    mutable std::mutex flush_mutex_;
    std::condition_variable flush_work_;
    std::deque<std::unique_ptr<DocumentsWriterPerThread>> flush_queue_;
    std::vector<std::thread> flush_workers_;
    DocumentsWriter::FlushSink flush_sink_;
    /// The first error a flush worker hit, handed to the next wait.
    std::optional<LuceneError> flush_error_;
    bool flush_closing_{false};
    std::condition_variable buffer_released_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::map<std::thread::id, size_t> slot_of_thread_;
    /// Buffers a caller is writing right now; they come back through
    /// `publish_flushed`.  Atomic, so the indexing path never takes the registry
    /// lock to update it.
    std::atomic<size_t> buffers_in_flight_{0};
    /// How many buffers may exist at once: the only backpressure of the writer.
    std::atomic<size_t> max_buffers_{1};
    /// Threads waiting for a free buffer.
    size_t stalled_writers_{0};
    /// Documents that live in a buffer a caller is writing.
    std::atomic<int64_t> in_flight_docs_{0};
    std::vector<SegmentCommitInfo> flushed_segments_;
    DocumentsWriter::DeleteQueue delete_queue_;
    /// Segments written out so far, reported by `flush_count`.
    int64_t flush_count_{0};
    bool delete_all_{false};
};

DocumentsWriter::DocumentsWriter(Directory &directory, const IndexWriterConfig &config,
                                 int64_t next_segment_counter)
    : impl_(std::make_unique<Impl>(directory, config, next_segment_counter)) {}

int64_t DocumentsWriter::DeleteQueue::add(BufferedDelete entry) {
    entries_.push_back(std::move(entry));
    return ++max_seq_no_;
}

DocumentsWriter::~DocumentsWriter() { (void)impl_->stop_flush_threads(); }

VoidResult DocumentsWriter::add_document(const Document &document) { return impl_->add_document(document); }

VoidResult DocumentsWriter::delete_documents(const Term &term, int64_t doc_upto) {
    impl_->delete_queue_.add(BufferedDelete{term, doc_upto, false});
    return {};
}


VoidResult DocumentsWriter::delete_all() {
    impl_->delete_all_ = true;
    return {};
}

Result<std::vector<SegmentCommitInfo>> DocumentsWriter::flush_all() { return impl_->flush_all(); }

Result<std::unique_ptr<DocumentsWriterPerThread>> DocumentsWriter::add_document_async(const Document &document) {
    return impl_->add_document_async(document);
}

VoidResult DocumentsWriter::publish_flushed(std::optional<SegmentCommitInfo> segment, int32_t flushed_docs) {
    return impl_->publish_flushed(std::move(segment), flushed_docs);
}

VoidResult DocumentsWriter::wait_for_in_flight_flushes() { return impl_->wait_for_in_flight_flushes(); }

void DocumentsWriter::start_flush_threads(DocumentsWriter::FlushSink sink, size_t count) {
    impl_->start_flush_threads(std::move(sink), count);
}

VoidResult DocumentsWriter::stop_flush_threads() { return impl_->stop_flush_threads(); }

bool DocumentsWriter::enqueue_flush(std::unique_ptr<DocumentsWriterPerThread> &buffer) {
    return impl_->enqueue_flush(buffer);
}

size_t DocumentsWriter::flush_thread_count() const noexcept { return impl_->flush_thread_count(); }

void DocumentsWriter::set_max_buffers(size_t count) noexcept { impl_->set_max_buffers(count); }

size_t DocumentsWriter::max_buffers() const noexcept { return impl_->max_buffers(); }

size_t DocumentsWriter::stalled_writers() const noexcept { return impl_->stalled_writers(); }

std::vector<SegmentCommitInfo> DocumentsWriter::take_flushed_segments() {
    return impl_->take_flushed_segments();
}

int64_t DocumentsWriter::bytes_used_in_ram() const noexcept { return impl_->bytes_used_in_ram(); }

int64_t DocumentsWriter::flush_count() const noexcept {
    return impl_->flush_count_;
}

size_t DocumentsWriter::pending_flush_count() const noexcept {
    return impl_->pending_flush_count();
}

bool DocumentsWriter::has_pending_flushes() const noexcept {
    return impl_->has_pending_flushes();
}

int64_t DocumentsWriter::pending_flushed_doc_count() const noexcept {
    return impl_->pending_flushed_doc_count();
}

int32_t DocumentsWriter::pending_doc_count() const noexcept {
    return impl_->pending_doc_count();
}

VoidResult DocumentsWriter::soft_delete_documents(const Term &term, int64_t doc_upto) {
    impl_->delete_queue_.add(BufferedDelete{term, doc_upto, true});
    return {};
}

const std::vector<DocumentsWriter::BufferedDelete> &DocumentsWriter::buffered_deletes() const noexcept {
    return impl_->delete_queue_.entries();
}

const DocumentsWriter::DeleteQueue &DocumentsWriter::delete_queue() const noexcept { return impl_->delete_queue_; }

bool DocumentsWriter::delete_all_pending() const noexcept { return impl_->delete_all_; }

void DocumentsWriter::clear_delete_buffer() noexcept {
    impl_->delete_queue_.clear();
    impl_->delete_all_ = false;
}

VoidResult DocumentsWriter::rollback() { return impl_->rollback(); }

int64_t DocumentsWriter::next_segment_counter() const noexcept {
    return impl_->next_segment_counter_.load();
}

int64_t DocumentsWriter::allocate_segment_counter() noexcept {
    return impl_->next_segment_counter_.fetch_add(1);
}

void DocumentsWriter::set_next_segment_counter(int64_t counter) noexcept {
    impl_->next_segment_counter_ = counter;
}

} // namespace lucene
