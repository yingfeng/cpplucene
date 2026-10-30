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

module lucene_core:index_writer.impl;

import :index_writer;

import :binary_doc_values;
import :bits;
import :codec;
import :codec_formats;
import :compound_format;
import :buffered_updates;
import :buffered_updates_stream;
import :directory;
import :doc_values_field_updates;
import :doc_values_format;
import :doc_values_producer;
import :documents_writer;
import :document;
import :error;
import :field_infos;
import :field_updates_buffer;
import :frozen_buffered_updates;
import :field_infos_format;
import :field_updates_buffer;
import :fixed_bit_set;
import :index_reader;
import :index_searcher;
import :index_deletion_policy;
import :index_file_deleter;
import :index_file_names;
import :index_writer_config;
import :index_writer_observers;
import :io_context;
import :leaf_reader;
import :lock;
import :lock_validating_directory_wrapper;
import :live_docs_format;
import :merge_policy;
import :merge_scheduler;
import :merge_trigger;
import :numeric_doc_values;
import :reader_pool;
import :readers_and_updates;
import :segment_commit_info;
import :segment_infos;
import :standard_directory_reader;
import :segment_merger;
import :segment_reader;
import :segment_write_state;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :sort_field;
import :string_helper;
import :term;
import :term_query;
import :top_docs;
import :types;
import :version;

import std;

namespace lucene {

namespace {

/// A file belongs to `segment` when it is `<segment>.<ext>` or
/// `<segment>_<something>.<ext>`.
[[nodiscard]] bool is_segment_file(std::string_view file, std::string_view segment) {
    if (file.size() <= segment.size() || file.substr(0, segment.size()) != segment) {
        return false;
    }
    const char separator = file[segment.size()];
    return separator == '.' || separator == '_';
}

/// The segment a file belongs to: `_0` for both `_0.tim` and `_0_1_Lucene90_0.dvd`.
///
/// A file whose name does not start with a segment name reports nothing, which is
/// how `delete_unused_files` skips the commit files.
[[nodiscard]] std::string segment_of(std::string_view file) {
    if (file.empty() || file.front() != '_') {
        return {};
    }
    const size_t second = file.find('_', 1);
    const size_t end = second != std::string_view::npos ? second : file.find('.');
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(file.substr(0, end));
}

/// Whether `file` is a commit file (`segments_N`, `pending_segments_N`, ...).
[[nodiscard]] bool is_commit_file_name(std::string_view file) {
    return file.starts_with("segments") || file.starts_with("pending_segments");
}

/// Files a segment owns, so a rolled back writer can delete them.
std::set<std::string> files_of(const SegmentInfos &infos) {
    std::set<std::string> files;
    for (const SegmentCommitInfo &segment : infos.segments) {
        files.insert(segment.info.files.begin(), segment.info.files.end());
    }
    return files;
}

/// The merged view of one numeric doc values field: the values the segment has,
/// with the buffered in-place updates laid over them.
///
/// Port of the `DocValuesProducerNumeric`/`DocValuesFieldUpdates` pair the
/// reference implementation builds in `ReadersAndUpdates::write_doc_values`; the
/// port resolves the documents at commit time instead of per document writer.
class OverlaidNumericDocValues final : public NumericDocValues {
public:
    OverlaidNumericDocValues(std::unique_ptr<NumericDocValues> base, const DocValuesFieldUpdates &updates,
                             int32_t max_doc)
        : base_(std::move(base)), updates_(&updates), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance_doc(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return advance_doc(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto doc = advance_doc(target);
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        return doc.value() == target;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (updates_->is_updated(doc_)) {
            auto value = updates_->numeric_value(doc_);
            if (!value.has_value()) {
                return err(LuceneError::illegal_state("doc " + std::to_string(doc_) +
                                                      " has no value, the update removed it"));
            }
            return value.value();
        }
        if (base_ == nullptr) {
            return err(LuceneError::illegal_state("doc " + std::to_string(doc_) + " has no value"));
        }
        return base_->long_value();
    }

private:
    [[nodiscard]] Result<int32_t> advance_doc(int32_t target) {
        while (target < max_doc_) {
            if (updates_->is_updated(target)) {
                // An update wins over the value the segment holds, and a removal
                // drops the document from the new generation.
                if (updates_->numeric_value(target).has_value()) {
                    doc_ = target;
                    return doc_;
                }
            } else {
                if (base_ != nullptr && base_->doc_id() < target) {
                    auto moved = base_->advance(target);
                    if (!moved.has_value()) {
                        return std::unexpected(std::move(moved.error()));
                    }
                }
                if (base_ != nullptr && base_->doc_id() == target) {
                    doc_ = target;
                    return doc_;
                }
            }
            ++target;
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    std::unique_ptr<NumericDocValues> base_;
    const DocValuesFieldUpdates *updates_;
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

/// The merged view of one binary doc values field; see
/// [`OverlaidNumericDocValues`].
class OverlaidBinaryDocValues final : public BinaryDocValues {
public:
    OverlaidBinaryDocValues(std::unique_ptr<BinaryDocValues> base, const DocValuesFieldUpdates &updates,
                            int32_t max_doc)
        : base_(std::move(base)), updates_(&updates), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance_doc(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return advance_doc(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto doc = advance_doc(target);
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        return doc.value() == target;
    }

    [[nodiscard]] Result<BytesRef> binary_value() override {
        if (updates_->is_updated(doc_)) {
            auto value = updates_->binary_value(doc_);
            if (!value.has_value()) {
                return err(LuceneError::illegal_state("doc " + std::to_string(doc_) +
                                                      " has no value, the update removed it"));
            }
            return value.value();
        }
        if (base_ == nullptr) {
            return err(LuceneError::illegal_state("doc " + std::to_string(doc_) + " has no value"));
        }
        return base_->binary_value();
    }

private:
    [[nodiscard]] Result<int32_t> advance_doc(int32_t target) {
        while (target < max_doc_) {
            if (updates_->is_updated(target)) {
                if (updates_->binary_value(target).has_value()) {
                    doc_ = target;
                    return doc_;
                }
            } else {
                if (base_ != nullptr && base_->doc_id() < target) {
                    auto moved = base_->advance(target);
                    if (!moved.has_value()) {
                        return std::unexpected(std::move(moved.error()));
                    }
                }
                if (base_ != nullptr && base_->doc_id() == target) {
                    doc_ = target;
                    return doc_;
                }
            }
            ++target;
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    std::unique_ptr<BinaryDocValues> base_;
    const DocValuesFieldUpdates *updates_;
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

/// The producer a doc values update writes with: it answers with the merged
/// values of the one field being written and refuses everything else.
class UpdateDocValuesProducer final : public DocValuesProducer {
public:
    /// `base_field` is the field as the segment recorded it, which is what the
    /// segment's own producer resolves its generation with; `field` carries the
    /// generation being written.
    UpdateDocValuesProducer(const DocValuesProducer &base, const FieldInfo &base_field, const FieldInfo &field,
                            const DocValuesFieldUpdates &updates, int32_t max_doc)
        : base_(&base), base_field_(base_field), field_(&field), updates_(&updates), max_doc_(max_doc) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        if (field.number != field_->number || updates_->sub_type() != DocValuesType::Numeric) {
            return err(LuceneError::unsupported_operation("only the updated numeric field is produced"));
        }
        auto base = base_->get_numeric(base_field_);
        if (!base.has_value()) {
            return std::unexpected(std::move(base.error()));
        }
        return std::unique_ptr<NumericDocValues>(
                new OverlaidNumericDocValues(std::move(base.value()), *updates_, max_doc_));
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        if (field.number != field_->number || updates_->sub_type() != DocValuesType::Binary) {
            return err(LuceneError::unsupported_operation("only the updated binary field is produced"));
        }
        auto base = base_->get_binary(base_field_);
        if (!base.has_value()) {
            return std::unexpected(std::move(base.error()));
        }
        return std::unique_ptr<BinaryDocValues>(
                new OverlaidBinaryDocValues(std::move(base.value()), *updates_, max_doc_));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("a doc values update never writes a sorted field"));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("a doc values update never writes a sorted numeric field"));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("a doc values update never writes a sorted set field"));
    }

    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const override {
        return std::unique_ptr<DocValuesProducer>(
                new UpdateDocValuesProducer(*base_, base_field_, *field_, *updates_, max_doc_));
    }

private:
    const DocValuesProducer *base_;
    FieldInfo base_field_;
    const FieldInfo *field_;
    const DocValuesFieldUpdates *updates_;
    int32_t max_doc_{0};
};

} // namespace

namespace {

/// The merge context a *background* merge hands to the merge policy.
///
/// It carries nothing of the writer: the packing decision only needs the
/// directory and the delete counts of the segments it is given, and a worker
/// must not read the writer's live segment list (another thread may be
/// appending to it).
class PackingMergeContext final : public MergeContext {
public:
    explicit PackingMergeContext(Directory &directory) : directory_(&directory) {}

    [[nodiscard]] Result<int32_t> num_deletes_to_merge(const SegmentCommitInfo &info) const override {
        return static_cast<int32_t>(info.total_del_count());
    }
    [[nodiscard]] Result<int32_t> num_deleted_docs(const SegmentCommitInfo &info) const override {
        return static_cast<int32_t>(info.total_del_count());
    }
    [[nodiscard]] InfoStreamMT get_info_stream() const override { return get_default_info_stream(); }
    [[nodiscard]] std::set<std::string> get_merging_segments() const override { return {}; }
    [[nodiscard]] Directory *directory() const override { return directory_; }

private:
    Directory *directory_;
};

} // namespace

struct IndexWriter::Impl : MergeSource {
    Impl(Directory &directory, IndexWriterConfig config, std::unique_ptr<Lock> write_lock)
        : write_lock_(std::move(write_lock)),
          // The writer talks to the lock-validating wrapper from here on, so a
          // delete or a write that happens after another process took the index
          // over fails before it does damage.  Reads stay unchecked.
          lock_wrapper_(write_lock_ != nullptr
                                ? std::make_unique<LockValidatingDirectoryWrapper>(directory, *write_lock_)
                                : nullptr),
          directory_orig_(directory),
          directory_(lock_wrapper_ != nullptr ? static_cast<Directory &>(*lock_wrapper_) : directory),
          reader_pool_(std::make_unique<ReaderPool>(directory_)),
          config_(std::move(config)) {}

    /// The writer's lock.  A background merge takes it itself, so a merge cannot
    /// rely on the caller holding it.
    std::recursive_mutex *mutex{nullptr};

    /// Set while the writer closes: a merge is no longer proposed or started, so
    /// a background worker cannot outlive the state it reads.
    bool closing_{false};

    /// Set while a forced merge runs: the commit it starts must not queue merges
    /// of its own, otherwise a background merge and a forced merge would fight
    /// over the same inputs.
    bool suppress_merges_{false};

    /// Whether the first phase of a two-phase commit ran and is waiting for its
    /// publication.
    bool commit_prepared_{false};

    /// The inputs of the merges a background worker is running right now.
    ///
    /// The policy has to see them as unmergeable: a queued merge is drained from
    /// `pending_merges_` as soon as a worker picks it up, so without this set the
    /// policy would propose a second merge over the same segments.
    std::set<std::string> merging_segments_;

    /// The merge context the policy sees: deletes come from the commit infos, the
    /// segments being merged are the queued ones, and sizes are measured against
    /// the writer's directory.
    class WriterMergeContext final : public MergeContext {
    public:
        explicit WriterMergeContext(const Impl &writer) : writer_(writer) {}

        [[nodiscard]] Result<int32_t> num_deletes_to_merge(const SegmentCommitInfo &info) const override {
            return static_cast<int32_t>(info.total_del_count());
        }

        [[nodiscard]] Result<int32_t> num_deleted_docs(const SegmentCommitInfo &info) const override {
            return static_cast<int32_t>(info.total_del_count());
        }

        [[nodiscard]] InfoStreamMT get_info_stream() const override { return get_default_info_stream(); }

        [[nodiscard]] std::set<std::string> get_merging_segments() const override {
            // Both halves matter: a merge that is only queued and one that a
            // background worker is already running must look the same to the
            // policy, otherwise it proposes a second merge over the same inputs.
            std::set<std::string> names = writer_.merging_segments_;
            for (const OneMerge &one_merge : writer_.pending_merges_) {
                for (const SegmentCommitInfo &segment : one_merge.segments()) {
                    names.insert(segment.info.name);
                }
            }
            return names;
        }

        [[nodiscard]] Directory *directory() const override { return &writer_.directory_; }

    private:
        const Impl &writer_;
    };

    [[nodiscard]] VoidResult drain_flushed_segments() {
        for (SegmentCommitInfo &segment : documents_writer_->take_flushed_segments()) {
            if (auto packed = maybe_write_compound_file(segment, nullptr); !packed.has_value()) {
                return packed;
            }
            if (auto added = infos_.add(std::move(segment)); !added.has_value()) {
                return added;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult flush() {
        auto flushed = documents_writer_->flush_all();
        if (!flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        for (SegmentCommitInfo &segment : flushed.value()) {
            if (auto packed = maybe_write_compound_file(segment, nullptr); !packed.has_value()) {
                return packed;
            }
            if (auto added = infos_.add(std::move(segment)); !added.has_value()) {
                return added;
            }
        }
        return {};
    }

    /// Writes a full indexing buffer out and queues the segment.
    ///
    /// It runs **without the writer lock**: the file I/O is what makes indexing
    /// parallel.  Packing a flushed segment only needs the configuration (a merge
    /// passes a context, a flush does not), so it happens here too.  The queue it
    /// fills is published by the next commit, like the reference implementation's
    /// flush queue.
    [[nodiscard]] VoidResult write_and_publish(DocumentsWriterPerThread &buffer) {
        const int32_t flushed_docs = buffer.num_docs_in_ram();
        auto flushed = buffer.flush();
        if (!flushed.has_value()) {
            (void)documents_writer_->publish_flushed(std::nullopt, flushed_docs);
            return std::unexpected(std::move(flushed.error()));
        }
        if (!flushed.value().has_value()) {
            return documents_writer_->publish_flushed(std::nullopt, flushed_docs);
        }
        if (auto packed = maybe_write_compound_file(flushed.value().value(), nullptr); !packed.has_value()) {
            (void)documents_writer_->publish_flushed(std::nullopt, flushed_docs);
            return packed;
        }
        return documents_writer_->publish_flushed(std::move(flushed.value()), flushed_docs);
    }

    /// Packs the files of `segment` into a compound file when the codec and the
    /// configuration ask for it.
    ///
    /// Port of the `useCompoundFile` step of the flush and merge paths: the
    /// segment's files are packed into `_N.cfs`/`_N.cfe`, the originals are
    /// deleted and the `.si` is rewritten so it names the compound files only.
    /// Reads then go through a [`CompoundDirectory`](:compound_directory), which
    /// keeps the number of open files at two per segment.
    ///
    /// `merge_context` is the merge that produced the segment, or null for a
    /// flush; only a merge consults
    /// [`MergePolicy::use_compound_file`](:merge_policy), which leaves a merge
    /// that is a large part of the index unpacked.
    [[nodiscard]] VoidResult maybe_write_compound_file(SegmentCommitInfo &segment, const MergeContext *merge_context,
                                                      const SegmentInfos *infos_view = nullptr) {
        if (!config_.use_compound_file()) {
            return {};
        }
        const CompoundFormat *compound = config_.codec().compound_format();
        if (compound == nullptr) {
            return {};
        }
        if (merge_context != nullptr &&
            !config_.merge_policy()->use_compound_file(infos_view != nullptr ? *infos_view : infos_, segment,
                                                       *merge_context)) {
            return {};
        }

        if (segment.info.use_compound_file) {
            // Already packed: a segment that a caller wrote out off the lock was
            // packed then, and packing it again would write the compound over
            // itself after the originals are gone.
            return {};
        }
        const std::string &name = segment.info.name;
        const std::string si_file = segment_file_name(name, "", "si");

        // The file list a flush leaves behind is not the complete set: the field
        // infos file and the live docs are derived from the segment's generations
        // rather than recorded, so the directory is what says which files belong
        // to the segment.
        // The live docs, the update generations of the field infos and the doc
        // values updates stay outside: a reader opens them from the plain
        // directory, because an update is written after the segment was packed.
        std::set<std::string> pack_files;
        auto listed = directory_.list_all();
        if (!listed.has_value()) {
            return std::unexpected(std::move(listed.error()));
        }
        const std::string base_field_infos = segment_file_name(name, "", "fnm");
        std::set<std::string> dv_update_files;
        for (const auto &[generation, files] : segment.doc_values_updates_files) {
            static_cast<void>(generation);
            dv_update_files.insert(files.begin(), files.end());
        }
        for (const std::string &file : listed.value()) {
            if (file == si_file || file.ends_with(".liv") || dv_update_files.contains(file)) {
                continue;
            }
            if (file.ends_with(".fnm") && file != base_field_infos) {
                continue;
            }
            if (file.starts_with(name + ".") || file.starts_with(name + "_")) {
                pack_files.insert(file);
            }
        }
        if (pack_files.empty()) {
            return {};
        }
        if (auto set = segment.info.set_files(pack_files); !set.has_value()) {
            return set;
        }
        if (auto written = compound->write(directory_, segment.info, io_context_default()); !written.has_value()) {
            return written;
        }

        // The compound files replace everything but the `.si`, which is rewritten
        // below.  A failure to delete an original is not fatal: the file stays as
        // an unreferenced leftover that the file deleter removes later.
        std::set<std::string> compound_files{si_file};
        for (const std::string &file : pack_files) {
            (void)directory_.delete_file(file);
        }
        for (const std::string_view extension : {std::string_view("cfs"), std::string_view("cfe")}) {
            compound_files.insert(segment_file_name(name, "", extension));
        }
        if (auto set = segment.info.set_files(std::move(compound_files)); !set.has_value()) {
            return set;
        }
        segment.info.use_compound_file = true;
        return config_.codec().segment_info_format().write(directory_, segment.info, io_context_default());
    }

    /// Marks every document matching a buffered delete as deleted.
    ///
    /// The reference implementation applies the deletes of the delete queue to each DWPT as they
    /// arrive; this port rewrites the live docs of the affected segments when
    /// the index is committed.
    /// Freezes what is buffered into a packet and registers it with the stream.
    ///
    /// **Deviation:** the reference implementation freezes the deletes of each
    /// document writer flush and applies the packet as soon as it is created; this
    /// port keeps applying its own buffers at commit time, so the packet is a
    /// snapshot that gives the deletes of a commit a generation and a size.  It
    /// has to be a snapshot, because applying the buffers consumes them.
    [[nodiscard]] VoidResult begin_updates_packet() {
        const bool any = !documents_writer_->buffered_deletes().empty() || !query_deletes_.empty() ||
                         !doc_deletes_.empty() || doc_values_updates_.any() || !doc_value_updates_.empty();
        if (!any) {
            current_packet_.reset();
            return {};
        }
        auto snapshot = std::make_unique<BufferedUpdates>();
        for (const DocumentsWriter::BufferedDelete &deleted : documents_writer_->buffered_deletes()) {
            if (auto added = snapshot->add_term(deleted.term, static_cast<int32_t>(deleted.doc_upto));
                !added.has_value()) {
                return added;
            }
        }
        for (const BufferedQueryDelete &deleted : query_deletes_) {
            if (auto added = snapshot->add_query(deleted.query, static_cast<int32_t>(deleted.doc_upto));
                !added.has_value()) {
                return added;
            }
        }
        for (const FieldUpdatesBuffer::Entry &entry : doc_values_updates_.entries()) {
            auto &buffer = snapshot->field_updates()[entry.field];
            if (entry.doc_values_type == DocValuesType::Numeric) {
                buffer.add_numeric(entry.term, entry.field, entry.doc_id_upto, entry.numeric_value);
            } else if (entry.doc_values_type == DocValuesType::Binary) {
                buffer.add_binary(entry.term, entry.field, entry.doc_id_upto, entry.binary_value);
            }
        }
        auto packet = FrozenBufferedUpdates::freeze(std::move(snapshot), std::nullopt);
        if (!packet.has_value()) {
            return std::unexpected(std::move(packet.error()));
        }
        auto pushed = updates_stream_->push(packet.value());
        if (!pushed.has_value()) {
            return std::unexpected(std::move(pushed.error()));
        }
        current_packet_ = packet.value();
        return {};
    }

    /// Records how much the packet deleted and marks it applied.
    void finish_updates_packet(int64_t deleted) {
        if (current_packet_ == nullptr) {
            return;
        }
        current_packet_->add_deleted(deleted);
        current_packet_->mark_applied();
        updates_stream_->set_applied_gen(current_packet_->del_gen());
        updates_stream_->prune_applied();
        current_packet_.reset();
    }

    /// Documents the segments hide, hard and soft deletions together.
    [[nodiscard]] int64_t hidden_doc_count() const {
        int64_t total = 0;
        for (const SegmentCommitInfo &segment : infos_.segments) {
            total += segment.del_count + segment.soft_del_count;
        }
        return total;
    }

    [[nodiscard]] VoidResult apply_deletes() {
        const std::vector<DocumentsWriter::BufferedDelete> &deletes = documents_writer_->buffered_deletes();
        const bool delete_all = documents_writer_->delete_all_pending();
        if (deletes.empty() && !delete_all && query_deletes_.empty() && doc_deletes_.empty()) {
            return {};
        }
        int64_t doc_base = 0;
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (auto applied = apply_deletes_to(segment, doc_base, deletes, delete_all); !applied.has_value()) {
                return applied;
            }
            doc_base += segment.max_doc();
        }
        query_deletes_.clear();
        return {};
    }

    /// Total documents the index holds, buffered ones included.
    ///
    /// A delete only applies to the documents that existed when it was issued,
    /// which is how `update_document` keeps the document it adds afterwards.
    [[nodiscard]] int64_t total_docs() const {
        // The queued flushed segments sit between `infos_` and the buffer, and the
        // deletes are cut off at this number, so they have to be counted.
        int64_t total = documents_writer_->pending_doc_count() + documents_writer_->pending_flushed_doc_count();
        for (const SegmentCommitInfo &segment : infos_.segments) {
            total += segment.max_doc();
        }
        return total;
    }

    [[nodiscard]] VoidResult apply_deletes_to(SegmentCommitInfo &segment, int64_t doc_base,
                                             const std::vector<DocumentsWriter::BufferedDelete> &deletes,
                                             bool delete_all) {
        const IOContext &context = io_context_default();
        // The pending deletions of the segment live in its `PendingDeletes`, which
        // starts from the committed `.liv` and only materialises a bit set once a
        // delete arrives.
        // One reader and one deletion state per segment, shared with the
        // near-real-time pool: reopening here would read the same `.liv`, `.fnm`
        // and dictionary once more.
        auto pooled = reader_pool_->get_readers_and_updates(segment, infos_.index_created_version_major, context);
        if (!pooled.has_value()) {
            return std::unexpected(std::move(pooled.error()));
        }
        std::shared_ptr<ReadersAndUpdates> state = std::move(pooled.value());
        std::shared_ptr<IndexReader> segment_reader = state->reader();
        PendingDeletes &pending = state->pending_deletes();

        // Documents cleared by a *soft* delete, which is only a report: the port
        // writes them into the `.liv` like a hard delete.
        FixedBitSet soft_cleared(static_cast<size_t>(segment.max_doc()));
        if (delete_all) {
            pending.delete_all();
        } else {
            std::shared_ptr<IndexReader> index_reader = segment_reader;
            // The searcher only reads, so the shared implementation of the
            // configured similarity can be handed over as a mutable pointer.
            std::shared_ptr<Similarity> similarity =
                    std::const_pointer_cast<Similarity>(config_.similarity());
            IndexSearcher searcher(std::move(index_reader), std::move(similarity));
            // The retention query decides which documents survive a soft delete: they
            // stay live and keep the data they were indexed with.
            FixedBitSet retained(static_cast<size_t>(segment.max_doc()));
            if (config_.soft_deletes_retention_query() != nullptr) {
                auto retained_hits = searcher.search(*config_.soft_deletes_retention_query(), segment.max_doc());
                if (!retained_hits.has_value()) {
                    return std::unexpected(std::move(retained_hits.error()));
                }
                for (const ScoreDoc &hit : retained_hits.value().score_docs) {
                    (void)retained.set(static_cast<size_t>(hit.doc));
                }
            }
            for (const DocumentsWriter::BufferedDelete &buffered : deletes) {
                TermQuery query(buffered.term);
                auto hits = searcher.search(query, segment.max_doc());
                if (!hits.has_value()) {
                    return std::unexpected(std::move(hits.error()));
                }
                for (const ScoreDoc &hit : hits.value().score_docs) {
                    if (doc_base + hit.doc >= buffered.doc_upto) {
                        // Indexed after the delete was issued.
                        continue;
                    }
                    if (buffered.soft && retained.get_bit(static_cast<size_t>(hit.doc))) {
                        // Retained by the retention query; not a delete at all.
                        continue;
                    }
                    auto was_live = pending.delete_doc(hit.doc);
                    if (!was_live.has_value()) {
                        return std::unexpected(std::move(was_live.error()));
                    }
                    if (was_live.value() && buffered.soft) {
                        (void)soft_cleared.set(static_cast<size_t>(hit.doc));
                    }
                }
            }
            for (const BufferedQueryDelete &buffered : query_deletes_) {
                auto hits = searcher.search(*buffered.query, segment.max_doc());
                if (!hits.has_value()) {
                    return std::unexpected(std::move(hits.error()));
                }
                for (const ScoreDoc &hit : hits.value().score_docs) {
                    if (doc_base + hit.doc >= buffered.doc_upto) {
                        continue;
                    }
                    if (auto deleted = pending.delete_doc(hit.doc); !deleted.has_value()) {
                        return std::unexpected(std::move(deleted.error()));
                    }
                }
            }
            for (const BufferedDocDelete &buffered : doc_deletes_) {
                // A single document delete only fits the segment it was issued on.
                if (buffered.segment != segment.info.name || doc_base + buffered.doc >= buffered.doc_upto ||
                    buffered.doc >= segment.max_doc()) {
                    continue;
                }
                if (auto deleted = pending.delete_doc(buffered.doc); !deleted.has_value()) {
                    return std::unexpected(std::move(deleted.error()));
                }
            }
        }

        // The soft-deleted documents keep their data, so the commit leaves the
        // generation of the delete in the soft deletes field: that is what tells a
        // soft delete from a hard one, and what a retention policy reads before it
        // lets a document go.  Reference: the `DocValuesFieldUpdates` a
        // `SoftDeletesDeleteQueue` node writes.
        if (const std::optional<std::string> &soft_deletes_field = config_.soft_deletes_field();
            soft_deletes_field.has_value() && soft_cleared.cardinality() > 0) {
            const int64_t generation = infos_.version + 1;
            for (size_t doc = 0; doc < soft_cleared.length(); ++doc) {
                if (!soft_cleared.get_bit(doc)) {
                    continue;
                }
                doc_value_updates_.push_back(BufferedDocValueUpdate{
                        segment.info.name, static_cast<int32_t>(doc), *soft_deletes_field,
                        NumericDocValuesUpdate::of(generation)});
            }
        }

        // `del_count` and `soft_del_count` are disjoint, together they are what the
        // `.liv` hides; the live docs format is handed the difference so its own
        // "the bits hold as many deletions as the commit says" check still holds.
        const int32_t total_deleted = segment.max_doc() - pending.num_live_docs();
        if (total_deleted == segment.del_count + segment.soft_del_count) {
            // Nothing new was deleted: forget the pending count, keep the state.
            pending.drop_changes();
            return {};
        }
        if (total_deleted == 0) {
            // Nothing is deleted any more: there is nothing to rewrite.
            segment.del_count = 0;
            segment.soft_del_count = 0;
            pending.drop_changes();
            return {};
        }
        const int32_t new_soft_count = static_cast<int32_t>(soft_cleared.cardinality());
        const int32_t new_hard_count = total_deleted - new_soft_count;
        auto written = state->write_live_docs(config_.codec().live_docs_format(), directory_, segment,
                                             total_deleted - segment.del_count - segment.soft_del_count, context);
        if (!written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        if (!written.value()) {
            pending.drop_changes();
            return {};
        }
        pending.drop_changes();
        segment.del_gen = segment.next_del_gen();
        segment.del_count = new_hard_count;
        segment.soft_del_count = new_soft_count;
        // The `.liv` is one of the segment's files: a merge, a rollback and an
        // import all have to see it.
        std::set<std::string> live_docs_files;
        if (auto listed = config_.codec().live_docs_format().files(segment, live_docs_files);
            !listed.has_value()) {
            return listed;
        }
        for (const std::string &file : live_docs_files) {
            if (auto added = segment.info.add_file(file); !added.has_value()) {
                return added;
            }
        }
        dirty_file_segments_.insert(segment.info.name);
        return {};
    }

    /// Applies the buffered doc values updates to every segment they touch.
    ///
    /// The reference implementation applies them to each pending document writer and
    /// writes a new generation per field through `ReadersAndUpdates`; this port
    /// walks the committed segments at commit time, resolves the documents of each
    /// buffered term and writes one generation per field.
    [[nodiscard]] VoidResult apply_doc_values_updates() {
        if (!doc_values_updates_.any() && doc_value_updates_.empty()) {
            return {};
        }
        int64_t doc_base = 0;
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (auto applied = apply_doc_values_updates_to(segment, doc_base); !applied.has_value()) {
                return applied;
            }
            doc_base += segment.max_doc();
        }
        doc_values_updates_.clear();
        return {};
    }

    [[nodiscard]] VoidResult apply_doc_values_updates_to(SegmentCommitInfo &segment, int64_t doc_base) {
        const IOContext &context = io_context_default();
        const DocValuesFormat *format = config_.codec().doc_values_format();
        if (format == nullptr) {
            return err(LuceneError::illegal_state("codec " + segment.info.codec_name +
                                                 " has no doc values format"));
        }

        auto opened = SegmentReader::open(directory_, segment, config_.index_created_version_major(), context);
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        std::shared_ptr<SegmentReader> reader = std::move(opened.value());
        auto infos = reader->field_infos();
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        // A mutable copy: the new generation of `.fnm` records the doc values
        // generation of every field that is written below.
        FieldInfos working = *infos.value();

        const DocValuesProducer *base_producer = reader->get_doc_values_reader();
        if (base_producer == nullptr) {
            // The segment has no doc values, so no update can apply to it.
            return {};
        }

        std::shared_ptr<IndexReader> index_reader = reader;
        std::shared_ptr<Similarity> similarity = std::const_pointer_cast<Similarity>(config_.similarity());
        IndexSearcher searcher(std::move(index_reader), std::move(similarity));

        auto listed = directory_.list_all();
        if (!listed.has_value()) {
            return std::unexpected(std::move(listed.error()));
        }
        const std::set<std::string> files_before(listed.value().begin(), listed.value().end());

        bool wrote_any = false;
        // The fields that have buffered updates, term based or per document.
        std::set<std::string> fields = doc_values_updates_.fields();
        for (const BufferedDocValueUpdate &buffered : doc_value_updates_) {
            if (buffered.segment == segment.info.name) {
                fields.insert(buffered.field);
            }
        }
        for (const std::string &field : fields) {
            const FieldInfo *info = working.by_name(field);
            if (info == nullptr) {
                // No document of this segment has the field.
                continue;
            }
            // The buffered type comes from the term based entries when there are
            // any, otherwise from the per-document ones.
            const auto buffered_type = [&]() -> DocValuesType {
                const DocValuesType by_term = doc_values_updates_.type_of(field);
                if (by_term != DocValuesType::None) {
                    return by_term;
                }
                for (const BufferedDocValueUpdate &buffered : doc_value_updates_) {
                    if (buffered.segment == segment.info.name && buffered.field == field) {
                        return std::holds_alternative<NumericDocValuesUpdate>(buffered.value)
                                       ? DocValuesType::Numeric
                                       : DocValuesType::Binary;
                    }
                }
                return DocValuesType::None;
            }();

            // A field that exists but stores no doc values cannot be updated, which
            // is what Lucene reports as well.
            if (auto checked = check_updatable(*info, buffered_type); !checked.has_value()) {
                return checked;
            }

            const int32_t max_doc = segment.max_doc();
            auto find_docs = [&searcher, max_doc](const Term &term) -> Result<std::vector<int32_t>> {
                TermQuery query(term);
                auto hits = searcher.search(query, max_doc);
                if (!hits.has_value()) {
                    return std::unexpected(std::move(hits.error()));
                }
                std::vector<int32_t> docs;
                docs.reserve(hits.value().score_docs.size());
                for (const ScoreDoc &hit : hits.value().score_docs) {
                    docs.push_back(hit.doc);
                }
                std::ranges::sort(docs);
                return docs;
            };

            std::unique_ptr<DocValuesFieldUpdates> updates;
            if (doc_values_updates_.type_of(field) != DocValuesType::None) {
                auto resolved = resolve_field_updates(doc_values_updates_, field, find_docs, doc_base);
                if (!resolved.has_value()) {
                    return std::unexpected(std::move(resolved.error()));
                }
                updates = std::move(resolved.value());
            } else {
                // Only per-document updates touch this field, so there is nothing to
                // resolve; the batch starts empty and the loop below fills it.
                if (buffered_type == DocValuesType::Numeric) {
                    updates = std::make_unique<NumericDocValuesFieldUpdates>(field, buffered_type);
                } else if (buffered_type == DocValuesType::Binary) {
                    updates = std::make_unique<BinaryDocValuesFieldUpdates>(field, buffered_type);
                } else {
                    continue;
                }
            }
            // A per-document update is already resolved: add it after the term
            // based ones, so it wins when both cover the same document.
            for (const BufferedDocValueUpdate &buffered : doc_value_updates_) {
                if (buffered.segment != segment.info.name || buffered.field != field) {
                    continue;
                }
                if (const auto *numeric = std::get_if<NumericDocValuesUpdate>(&buffered.value);
                    numeric != nullptr) {
                    std::optional<int64_t> value;
                    if (numeric->has_value()) {
                        value = numeric->value().value();
                    }
                    if (auto added = updates->add_numeric(buffered.doc, value); !added.has_value()) {
                        return added;
                    }
                } else {
                    const auto &binary = std::get<BinaryDocValuesUpdate>(buffered.value);
                    std::optional<BytesRef> value;
                    if (binary.has_value()) {
                        value = binary.value().value();
                    }
                    if (auto added = updates->add_binary(buffered.doc, value); !added.has_value()) {
                        return added;
                    }
                }
            }
            updates->finish();
            if (updates->size() == 0) {
                continue;
            }
            if (auto written = write_doc_values_generation(segment, working, *info, *updates, *format,
                                                          *base_producer, context);
                !written.has_value()) {
                return written;
            }
            wrote_any = true;
        }

        if (!wrote_any) {
            return {};
        }

        // The new generation of `.fnm` is what tells a reader which doc values
        // generation each field lives in.
        const int64_t infos_gen = segment.next_field_infos_gen();
        if (auto written = config_.codec().field_infos_format().write(
                    directory_, segment.info, IndexFileNames::base36_encode(infos_gen), working, context);
            !written.has_value()) {
            return written;
        }
        segment.advance_field_infos_gen();

        // Everything the update created belongs to the segment: the commit has to
        // list it, and a merged-away segment has to delete it.
        auto after = directory_.list_all();
        if (!after.has_value()) {
            return std::unexpected(std::move(after.error()));
        }
        for (const std::string &name : after.value()) {
            if (!files_before.contains(name) && is_segment_file(name, segment.info.name)) {
                if (auto added = segment.info.add_file(name); !added.has_value()) {
                    return added;
                }
            }
        }
        dirty_file_segments_.insert(segment.info.name);

        // A pooled reader still describes the previous generation.
        reader_pool_->drop(segment);
        return {};
    }

    /// The field has to be a plain numeric or binary doc values field; Lucene
    /// rejects a sorted numeric or a sorted set field the same way.
    [[nodiscard]] static VoidResult check_updatable(const FieldInfo &info, DocValuesType update_type) {
        const DocValuesType stored = info.doc_values_type;
        const bool numeric = update_type == DocValuesType::Numeric;
        const bool ok = numeric ? stored == DocValuesType::Numeric : stored == DocValuesType::Binary;
        if (!ok) {
            return err(LuceneError::illegal_argument(
                    "cannot update the doc values of field \"" + info.name + "\" in place: " +
                    (numeric ? "a numeric update needs a NUMERIC field" : "a binary update needs a BINARY field")));
        }
        return {};
    }

    /// Writes one doc values generation holding `field`, with the updates applied.
    [[nodiscard]] VoidResult write_doc_values_generation(SegmentCommitInfo &segment, FieldInfos &infos,
                                                         const FieldInfo &info,
                                                         const DocValuesFieldUpdates &updates,
                                                         const DocValuesFormat &format,
                                                         const DocValuesProducer &base_producer,
                                                         const IOContext &context) {
        const int64_t gen = segment.next_doc_values_gen();
        // `info` aliases the field inside `infos`, so the generation the segment
        // holds now has to be copied before it is overwritten below.
        const FieldInfo original = info;
        // The field records the generation it is about to be written with, which is
        // what the per-field writer turns into the file suffix.
        FieldInfo *mutable_info = infos.field_info_mut(info.name);
        if (mutable_info == nullptr) {
            return err(LuceneError::illegal_state("field \"" + info.name + "\" disappeared"));
        }
        mutable_info->doc_values_gen = gen;

        // A generation holds only the field it updates, like the reference
        // implementation, which writes one generation per field.
        std::shared_ptr<const FieldInfos> restricted =
                std::make_shared<const FieldInfos>(infos.with_filter({info.name}));
        SegmentWriteState state(get_default_info_stream(), directory_, std::move(restricted), context,
                                IndexFileNames::base36_encode(gen));
        auto consumer = format.fields_consumer(state, segment.info);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }

        UpdateDocValuesProducer producer(base_producer, original, *mutable_info, updates, segment.max_doc());
        if (updates.sub_type() == DocValuesType::Numeric) {
            if (auto added = consumer.value()->add_numeric_field(*mutable_info, producer); !added.has_value()) {
                return added;
            }
        } else {
            if (auto added = consumer.value()->add_binary_field(*mutable_info, producer); !added.has_value()) {
                return added;
            }
        }
        if (auto closed = consumer.value()->close(); !closed.has_value()) {
            return closed;
        }

        segment.advance_doc_values_gen();
        return {};
    }

    /// Bytes the merge reads and writes, used for the merge `IOContext`.
    [[nodiscard]] Result<int64_t> estimated_merge_bytes(const OneMerge &merge) const {
        int64_t total = 0;
        for (const SegmentCommitInfo &segment : merge.segments()) {
            for (const std::string &file : segment.info.files) {
                auto length = directory_.file_length(file);
                if (!length.has_value()) {
                    return std::unexpected(std::move(length.error()));
                }
                total += static_cast<int64_t>(length.value());
            }
        }
        return total;
    }

    /// Runs one merge and returns the segment it produced.
    ///
    /// `nullopt` means the merge produced no document, which happens when every
    /// input document is deleted: the inputs are then simply dropped.
    /// The part of a merge that has to read the writer state: the name, the id
    /// and the size estimate of the segment about to be written.
    struct MergePlan {
        SegmentInfo info;
        int64_t estimated_bytes{0};
        /// The version the index was created with, needed to open the inputs.
        int32_t created_version_major{0};
        /// The segments the index held when the merge was taken, so the packing
        /// decision does not have to read the live list from a worker thread.
        std::shared_ptr<const SegmentInfos> index_snapshot;
    };

    /// Takes everything a merge needs from the writer, so the I/O that follows
    /// can run without the lock.
    [[nodiscard]] Result<MergePlan> make_merge_plan(const OneMerge &merge) {
        auto bytes = estimated_merge_bytes(merge);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        MergePlan plan;
        plan.estimated_bytes = bytes.value();
        plan.created_version_major = infos_.index_created_version_major;
        plan.index_snapshot = std::make_shared<const SegmentInfos>(infos_);

        SegmentInfo merged_info;
        // Take the name from the shared counter atomically: a flush running on
        // another thread allocates from the same counter.
        merged_info.name = "_" + IndexFileNames::base36_encode(documents_writer_->allocate_segment_counter());
        infos_.counter = std::max(infos_.counter, documents_writer_->next_segment_counter());
        const auto random = StringHelper::random_id();
        merged_info.id.assign(random.begin(), random.end());
        merged_info.version = LUCENE_CURRENT_VERSION;
        merged_info.min_version = LUCENE_CURRENT_VERSION;
        merged_info.codec_name = std::string(config_.codec().name());
        merged_info.diagnostics.emplace("source", "merge");
        merged_info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());

        // A merge keeps the index sort, but only when every input is sorted by it:
        // otherwise the merged documents would not be in that order and the
        // metadata would lie.  The merger then reorders the documents itself.
        if (config_.index_sort().has_value()) {
            bool sorted_like_config = true;
            for (const SegmentCommitInfo &segment : merge.segments()) {
                if (!segment.info.sort.has_value() || !segment.info.sort.value().equals(config_.index_sort().value())) {
                    sorted_like_config = false;
                    break;
                }
            }
            if (sorted_like_config) {
                merged_info.sort = config_.index_sort().value();
            }
        }
        plan.info = std::move(merged_info);
        return plan;
    }

    /// The I/O half of a merge: opens the input segments and writes the result.
    ///
    /// It touches no writer state, so a background scheduler can run it while
    /// the writer keeps accepting documents.
    [[nodiscard]] Result<std::optional<SegmentCommitInfo>> write_merge_io(const OneMerge &merge,
                                                                        const MergePlan &plan) {
        std::vector<std::shared_ptr<SegmentReader>> readers;
        std::vector<CodecReader *> raw_readers;
        readers.reserve(merge.size());
        raw_readers.reserve(merge.size());
        for (const SegmentCommitInfo &segment : merge.segments()) {
            auto reader = SegmentReader::open(directory_, segment, plan.created_version_major, io_context_default());
            if (!reader.has_value()) {
                return std::unexpected(std::move(reader.error()));
            }
            raw_readers.push_back(reader.value().get());
            readers.push_back(std::move(reader.value()));
        }
        if (raw_readers.empty()) {
            return err(LuceneError::illegal_state("a merge needs at least one segment"));
        }
        return write_merged_segment(plan.info, raw_readers, merge.total_max_doc(), plan.estimated_bytes,
                                    static_cast<int32_t>(merge.size()), plan.index_snapshot.get());
    }

    /// Writes a brand-new segment out of `raw_readers` and returns its commit.
    ///
    /// Shared by the merge path (whose readers come from `infos_`) and
    /// [`add_indexes_from_codec_readers`], whose readers come from another index
    /// and are merged in instead of being linked.
    [[nodiscard]] Result<std::optional<SegmentCommitInfo>>
    write_merged_segment(SegmentInfo merged_info, std::span<CodecReader *const> raw_readers,
                         int64_t total_max_doc, int64_t estimated_bytes, int32_t num_sources,
                         const SegmentInfos *index_snapshot = nullptr) {
        auto merge_context = IOContext::with_merge(MergeInfo(static_cast<int32_t>(total_max_doc), estimated_bytes, false, num_sources));
        if (!merge_context.has_value()) {
            return std::unexpected(std::move(merge_context.error()));
        }

        auto merger = SegmentMerger::create(raw_readers, merged_info, get_default_info_stream(), directory_,
                                            merge_context.value());
        if (!merger.has_value()) {
            delete_partial_segment(merged_info.name);
            return std::unexpected(std::move(merger.error()));
        }
        if (!merger.value()->should_merge()) {
            // There is nothing left to write; the caller drops the inputs.
            return std::optional<SegmentCommitInfo>{};
        }
        if (auto merged = merger.value()->merge(); !merged.has_value()) {
            // A half written segment must not be left behind: the inputs are
            // untouched, so only the output of this merge has to go.
            delete_partial_segment(merged_info.name);
            return std::unexpected(std::move(merged.error()));
        }

        SegmentCommitInfo commit;
        commit.info = merged_info;
        // The file list goes into both `.si` and `segments_N`, so it is collected
        // before the metadata is written.
        auto names = directory_.list_all();
        if (!names.has_value()) {
            return std::unexpected(std::move(names.error()));
        }
        std::set<std::string> files;
        for (const std::string &name : names.value()) {
            if (is_segment_file(name, merged_info.name)) {
                files.insert(name);
            }
        }
        if (auto set = commit.info.set_files(std::move(files)); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto written = config_.codec().segment_info_format().write(directory_, commit.info, io_context_default());
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        {
            // Packing is a merge decision: a merge that is a large part of the
            // index stays unpacked, a small one becomes a compound segment.  A
            // merge that runs on a worker thread decides from the snapshot it took
            // when it started, never from the live segment list.
            const PackingMergeContext merge_policy_context(directory_);
            SegmentInfos snapshot;
            if (index_snapshot != nullptr) {
                snapshot = *index_snapshot;
            } else {
                snapshot = infos_;
            }
            if (auto packed = maybe_write_compound_file(commit, &merge_policy_context, &snapshot); !packed.has_value()) {
                delete_partial_segment(merged_info.name);
                return std::unexpected(std::move(packed.error()));
            }
        }
        // Warm the new segment before it becomes visible, like the reference's
        // `IndexWriterConfig::set_merged_segment_warmer`.
        if (const std::shared_ptr<IndexReaderWarmer> &warmer = config_.merged_segment_warmer(); warmer != nullptr) {
            auto reader = SegmentReader::open(directory_, commit, infos_.index_created_version_major,
                                              io_context_default());
            if (!reader.has_value()) {
                return std::unexpected(std::move(reader.error()));
            }
            if (auto warmed = warmer->warm(*reader.value()); !warmed.has_value()) {
                return std::unexpected(std::move(warmed.error()));
            }
        }
        return std::optional<SegmentCommitInfo>(std::move(commit));
    }

    /// Deletes every file of `segment` that already reached the directory.
    ///
    /// The reference implementation gets the same effect by writing the merge
    /// through temporary names (`TrackingTmpOutputDirectory`); this port writes
    /// the final names directly, so a failed merge cleans up by prefix instead.
    void delete_partial_segment(std::string_view segment) {
        auto names = directory_.list_all();
        if (!names.has_value()) {
            return;
        }
        for (const std::string &name : names.value()) {
            if (is_segment_file(name, segment)) {
                (void)directory_.delete_file(name);
            }
        }
    }

    /// Replaces the input segments of `merge` with `merged`, or just drops them
    /// when the merge produced no document.
    void register_merged_segment(const OneMerge &merge, std::optional<SegmentCommitInfo> merged) {
        std::set<std::string> replaced;
        for (const SegmentCommitInfo &segment : merge.segments()) {
            replaced.insert(segment.info.name);
            orphan_files_.insert(segment.info.files.begin(), segment.info.files.end());
        }

        std::vector<SegmentCommitInfo> segments;
        segments.reserve(infos_.segments.size());
        size_t insert_at = infos_.segments.size();
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (replaced.contains(segment.info.name)) {
                insert_at = std::min(insert_at, segments.size());
                continue;
            }
            segments.push_back(std::move(segment));
        }
        if (insert_at > segments.size()) {
            insert_at = segments.size();
        }
        if (merged.has_value()) {
            segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(merged.value()));
        }
        infos_.segments = std::move(segments);
    }

    // ---- MergeSource ----

    [[nodiscard]] Result<std::optional<OneMerge>> get_next_merge() override {
        if (pending_merges_.empty()) {
            return std::optional<OneMerge>{};
        }
        OneMerge next = std::move(pending_merges_.front());
        pending_merges_.erase(pending_merges_.begin());
        return std::optional<OneMerge>(std::move(next));
    }

    [[nodiscard]] Result<bool> has_pending_merges() override { return !pending_merges_.empty(); }

    /// Runs one merge in three steps so a background scheduler can overlap the
    /// file I/O with the indexing the caller does next.
    ///
    /// 1. **Take** (under the writer lock): everything the I/O needs from the
    ///    writer state, plus a file reference per input so a commit that runs
    ///    while the merge writes cannot delete them.
    /// 2. **Write** (no writer lock): open the inputs and write the merged
    ///    segment.
    /// 3. **Publish** (under the writer lock): only when every input is still
    ///    part of the index, otherwise the merged files are dropped again.
    ///
    /// A serial scheduler calls this while the caller already holds the lock, so
    /// the I/O keeps running under it (the lock is recursive); a background
    /// scheduler calls it from a worker, so the lock really is released while the
    /// merge writes.
    [[nodiscard]] VoidResult merge(OneMerge one_merge) override {
        std::vector<std::string> input_files;
        MergePlan plan;
        {
            const std::lock_guard<std::recursive_mutex> guard(*mutex);
            auto made = make_merge_plan(one_merge);
            if (!made.has_value()) {
                return std::unexpected(std::move(made.error()));
            }
            plan = std::move(made.value());

            for (const SegmentCommitInfo &segment : one_merge.segments()) {
                if (merging_segments_.contains(segment.info.name)) {
                    // Another worker is already merging this segment.  Doing the
                    // merge again would consume the same inputs twice and both
                    // results would be discarded (each would find its inputs gone),
                    // so the proposal is dropped instead.
                    return {};
                }
            }
            for (const SegmentCommitInfo &segment : one_merge.segments()) {
                merging_segments_.insert(segment.info.name);
            }
            SegmentInfos inputs;
            inputs.segments = one_merge.segments();
            input_files = segment_files(inputs);
            if (deleter_ != nullptr) {
                if (auto referenced = deleter_->inc_ref(input_files); !referenced.has_value()) {
                    return referenced;
                }
            }
        }

        auto merged = write_merge_io(one_merge, plan);

        const std::lock_guard<std::recursive_mutex> guard(*mutex);
        for (const SegmentCommitInfo &segment : one_merge.segments()) {
            merging_segments_.erase(segment.info.name);
        }
        if (merged.has_value() && merged.value().has_value() && !merge_inputs_present(one_merge)) {
            // A commit and a merge cannot remove an input (the file references
            // above keep it alive), but `delete_all` can: the merged segment is
            // then an orphan and its files go away with the reference.
            for (const std::string &file : merged.value().value().info.files) {
                (void)directory_.delete_file(file);
            }
            merged = Result<std::optional<SegmentCommitInfo>>(std::optional<SegmentCommitInfo>{});
        }
        VoidResult result;
        if (!merged.has_value()) {
            result = std::unexpected(std::move(merged.error()));
        } else {
            register_merged_segment(one_merge, std::move(merged.value()));
        }
        if (deleter_ != nullptr) {
            if (auto released = deleter_->dec_ref(input_files); !released.has_value() && result.has_value()) {
                result = released;
            }
        }
        return result;
    }

    /// Whether every input of `merge` is still in the index.
    [[nodiscard]] bool merge_inputs_present(const OneMerge &merge) const {
        for (const SegmentCommitInfo &input : merge.segments()) {
            const bool found = std::ranges::any_of(infos_.segments, [&input](const SegmentCommitInfo &segment) {
                return segment.info.name == input.info.name;
            });
            if (!found) {
                return false;
            }
        }
        return true;
    }

    // ---- merging ----

    /// Asks the policy for the merges that are necessary now and queues them.
    [[nodiscard]] VoidResult update_pending_merges(MergeTrigger trigger) {
        if (!pending_merges_.empty()) {
            return {};
        }
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_merges(trigger, infos_, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Queues the merges a forced merge asks for.
    [[nodiscard]] VoidResult update_pending_forced_merges(int32_t max_num_segments) {
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_forced_merges(infos_, max_num_segments, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Queues the merges that expunge deletes.
    [[nodiscard]] VoidResult update_pending_forced_deletes_merges() {
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_forced_deletes_merges(infos_, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Hands the queued merges to the scheduler.
    ///
    /// The configured [`IndexWriterEventListener`] is told before and after, which
    /// is what the reference implementation does around the merges of a full flush.
    [[nodiscard]] VoidResult run_scheduler(MergeTrigger trigger) {
        if (closing_) {
            pending_merges_.clear();
            return {};
        }
        if (pending_merges_.empty()) {
            return {};
        }
        const std::shared_ptr<MergeScheduler> &scheduler = config_.merge_scheduler();
        if (scheduler == nullptr) {
            return {};
        }

        std::vector<MergeStat> stats = merge_stats();
        notify_merge_begin(stats);
        VoidResult result = scheduler->merge(*this, trigger);
        notify_merge_end(stats);
        return result;
    }

    /// Asks the policy for merges and runs them.  Port of `IndexWriter::maybeMerge`.
    [[nodiscard]] VoidResult maybe_merge(MergeTrigger trigger) {
        if (closing_ || suppress_merges_) {
            // Closing the writer must not start a merge whose result would never
            // be committed.
            pending_merges_.clear();
            return {};
        }
        if (auto updated = update_pending_merges(trigger); !updated.has_value()) {
            return updated;
        }
        return run_scheduler(trigger);
    }

    /// Runs forced merges until the policy stops proposing them.
    [[nodiscard]] VoidResult force_merge(int32_t max_num_segments) {
        // Every round removes at least one segment, so the number of segments is
        // a safe bound.
        size_t remaining_rounds = infos_.segments.size() + 1;
        while (remaining_rounds-- > 0) {
            const size_t before = pending_merges_.size();
            if (auto queued = update_pending_forced_merges(max_num_segments); !queued.has_value()) {
                return queued;
            }
            if (pending_merges_.size() == before) {
                return {};
            }
            if (auto merged = run_merges_inline(); !merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    /// Runs the queued merges on this thread.
    ///
    /// A forced merge is synchronous in the reference implementation as well
    /// (`forceMerge` waits for the merges it started), and running it here keeps
    /// it from competing with a background merge over the same inputs.
    [[nodiscard]] VoidResult run_merges_inline() {
        const std::vector<MergeStat> stats = merge_stats();
        notify_merge_begin(stats);
        VoidResult result;
        while (true) {
            auto next = get_next_merge();
            if (!next.has_value()) {
                result = std::unexpected(std::move(next.error()));
                break;
            }
            if (!next.value().has_value()) {
                break;
            }
            if (auto merged = merge(std::move(next.value().value())); !merged.has_value()) {
                result = merged;
                break;
            }
        }
        notify_merge_end(stats);
        return result;
    }

    /// The merges that are queued right now, as the event listener wants them.
    [[nodiscard]] std::vector<MergeStat> merge_stats() const {
        std::vector<MergeStat> stats;
        stats.reserve(pending_merges_.size());
        for (const OneMerge &one_merge : pending_merges_) {
            MergeStat stat;
            stat.merge_gen = infos_.counter;
            for (const SegmentCommitInfo &segment : one_merge.segments()) {
                stat.segments.push_back(MergeStat::Segment{segment.info.name, segment.info.id});
            }
            stats.push_back(std::move(stat));
        }
        return stats;
    }

    void notify_merge_begin(const std::vector<MergeStat> &stats) const {
        const std::shared_ptr<IndexWriterEventListener> &listener = config_.event_listener();
        if (listener != nullptr) {
            listener->begin_merge_on_full_flush(stats);
        }
    }

    void notify_merge_end(const std::vector<MergeStat> &stats) const {
        const std::shared_ptr<IndexWriterEventListener> &listener = config_.event_listener();
        if (listener != nullptr) {
            listener->end_merge_on_full_flush(stats);
        }
    }

    [[nodiscard]] VoidResult force_merge_deletes() {
        size_t remaining_rounds = infos_.segments.size() + 1;
        while (remaining_rounds-- > 0) {
            const size_t before = pending_merges_.size();
            if (auto queued = update_pending_forced_deletes_merges(); !queued.has_value()) {
                return queued;
            }
            if (pending_merges_.size() == before) {
                return {};
            }
            if (auto merged = run_merges_inline(); !merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    /// Rewrites the `.si` of every segment that gained files since it was written.
    [[nodiscard]] VoidResult refresh_dirty_segment_info() {
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (!dirty_file_segments_.contains(segment.info.name)) {
                continue;
            }
            if (auto written = config_.codec().segment_info_format().write(directory_, segment.info,
                                                                          io_context_default());
                !written.has_value()) {
                return written;
            }
        }
        dirty_file_segments_.clear();
        doc_value_updates_.clear();
        return {};
    }

    /// Copies the segments of `source` in, under fresh names.
    /// Merges `readers` into one new segment; returns how many documents it added.
    ///
    /// The readers come from another index (or from memory), so they cannot be
    /// linked the way [`add_indexes_from_directory`] links segment files: their
    /// documents are written again through a merge.
    [[nodiscard]] Result<int64_t> add_indexes_from_codec_readers(
            const std::vector<std::shared_ptr<CodecReader>> &readers) {
        if (readers.empty()) {
            return int64_t{0};
        }
        std::vector<CodecReader *> raw_readers;
        raw_readers.reserve(readers.size());
        int64_t num_docs = 0;
        for (const std::shared_ptr<CodecReader> &reader : readers) {
            if (reader == nullptr) {
                return err(LuceneError::illegal_argument("an added reader must not be null"));
            }
            auto docs = reader->num_docs();
            if (!docs.has_value()) {
                return std::unexpected(std::move(docs.error()));
            }
            raw_readers.push_back(reader.get());
            num_docs += docs.value();
        }
        if (num_docs == 0) {
            return int64_t{0};
        }

        SegmentInfo merged_info;
        // Take the name from the shared counter atomically: a flush running on
        // another thread allocates from the same counter.
        merged_info.name = "_" + IndexFileNames::base36_encode(documents_writer_->allocate_segment_counter());
        infos_.counter = std::max(infos_.counter, documents_writer_->next_segment_counter());
        const auto random = StringHelper::random_id();
        merged_info.id.assign(random.begin(), random.end());
        merged_info.version = LUCENE_CURRENT_VERSION;
        merged_info.min_version = LUCENE_CURRENT_VERSION;
        merged_info.codec_name = std::string(config_.codec().name());
        merged_info.diagnostics.emplace("source", "addIndexes(CodecReaders...)");
        merged_info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());

        // **Deviation:** the reference implementation asks the merge policy for a
        // specification over the readers; this port merges them into one segment,
        // which the policy would also be allowed to produce.
        auto merged = write_merged_segment(std::move(merged_info), raw_readers, num_docs, /*estimated_bytes=*/0,
                                          static_cast<int32_t>(raw_readers.size()));
        if (!merged.has_value()) {
            return std::unexpected(std::move(merged.error()));
        }
        if (merged.value().has_value()) {
            infos_.segments.push_back(std::move(merged.value().value()));
        }
        return num_docs;
    }

    /// Writes the entries of a copied compound file back out under the segment's
    /// new name.
    ///
    /// The compound's entries keep the file names they were packed with, so a
    /// compound copied to `_N` still holds `_M.fnm`.  Unpacking it into files
    /// named after `_N` (and dropping the compound itself) is what
    /// [`add_indexes_from_directory`] needs; the caller repacks the segment
    /// afterwards.
    ///
    /// `segment.info.name` is already the new name here, so the reader is opened
    /// under it and lists the entries as `_N.fnm`, `_N_Lucene90_0.tim`, ...  Those
    /// are exactly the names the unpacked files must have.
    [[nodiscard]] VoidResult unpack_compound_segment(SegmentCommitInfo &segment, const IOContext &context) {
        const Codec *codec = config_.codec().name() == segment.info.codec_name
                                     ? &config_.codec()
                                     : CodecFormats::for_name(segment.info.codec_name);
        if (codec == nullptr || codec->compound_format() == nullptr) {
            return err(LuceneError::illegal_state("codec " + segment.info.codec_name +
                                                  " does not support compound files"));
        }
        auto compound = codec->compound_format()->get_compound_reader(directory_, segment.info);
        if (!compound.has_value()) {
            return std::unexpected(std::move(compound.error()));
        }
        auto entries = compound.value()->list_all();
        if (!entries.has_value()) {
            return std::unexpected(std::move(entries.error()));
        }

        // Everything the copy brought along except the compound pair itself stays:
        // the live docs and the update generations are not inside the compound.
        std::set<std::string> files;
        for (const std::string &file : segment.info.files) {
            if (!file.ends_with(".cfs") && !file.ends_with(".cfe")) {
                files.insert(file);
            }
        }
        files.insert(segment_file_name(segment.info.name, "", "si"));
        std::vector<char> buffer(64 * 1024);
        for (const std::string &entry : entries.value()) {
            const std::string &renamed = entry;
            auto input = compound.value()->open_input(entry, context);
            if (!input.has_value()) {
                return std::unexpected(std::move(input.error()));
            }
            auto output = directory_.create_output(renamed, context);
            if (!output.has_value()) {
                return std::unexpected(std::move(output.error()));
            }
            auto input_length = input.value()->length();
            if (!input_length.has_value()) {
                return std::unexpected(std::move(input_length.error()));
            }
            const int64_t length = static_cast<int64_t>(input_length.value());
            for (int64_t copied = 0; copied < length;) {
                const size_t chunk = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(buffer.size()),
                                                                          length - copied));
                const std::span<uint8_t> window(reinterpret_cast<uint8_t *>(buffer.data()), chunk);
                if (auto read = input.value()->read_bytes(window); !read.has_value()) {
                    return read;
                }
                if (auto written = output.value()->write_bytes(std::span<const uint8_t>(window)); !written.has_value()) {
                    return written;
                }
                copied += static_cast<int64_t>(chunk);
            }
            if (auto closed = output.value()->close(); !closed.has_value()) {
                return closed;
            }
            files.insert(renamed);
        }
        for (const std::string &file : segment.info.files) {
            if (!files.contains(file)) {
                (void)directory_.delete_file(file);
            }
        }
        segment.info.use_compound_file = false;
        return segment.info.set_files(std::move(files));
    }

    [[nodiscard]] VoidResult add_indexes_from_directory(Directory &source) {
        if (&source == &directory_orig_) {
            return err(LuceneError::illegal_argument("cannot add an index to itself"));
        }
        auto source_infos = SegmentInfos::read_latest(source);
        if (!source_infos.has_value()) {
            return err(source_infos.error());
        }
        if (source_infos.value().segments.empty()) {
            // An empty index: nothing to copy.
            return {};
        }

        std::set<std::string> taken;
        for (const SegmentCommitInfo &segment : infos_.segments) {
            taken.insert(segment.info.name);
        }
        int64_t counter = documents_writer_->next_segment_counter();
        const IOContext &context = io_context_default();

        for (const SegmentCommitInfo &source_segment : source_infos.value().segments) {
            // A fresh, unused name; `_<counter>` is what the writer would hand out.
            std::string name;
            do {
                name = "_" + IndexFileNames::base36_encode(counter++);
            } while (taken.contains(name));
            taken.insert(name);

            const std::string &old_name = source_segment.info.name;

            // Everything the segment owns: its own files plus the live docs, which
            // the commit info names separately.
            std::set<std::string> source_files(source_segment.info.files.begin(), source_segment.info.files.end());
            if (auto listed = config_.codec().live_docs_format().files(source_segment, source_files);
                !listed.has_value()) {
                return listed;
            }
            // The update generations are recorded apart from the codec files.
            source_files.insert(source_segment.field_infos_files.begin(), source_segment.field_infos_files.end());
            for (const auto &[generation, files] : source_segment.doc_values_updates_files) {
                static_cast<void>(generation);
                source_files.insert(files.begin(), files.end());
            }

            SegmentCommitInfo target;
            target.info = source_segment.info;
            target.info.name = name;
            target.del_gen = source_segment.del_gen;
            target.del_count = source_segment.del_count;
            target.soft_del_count = source_segment.soft_del_count;
            target.field_infos_gen = source_segment.field_infos_gen;
            target.doc_values_gen = source_segment.doc_values_gen;
            target.id = source_segment.id;

            std::set<std::string> copied;
            for (const std::string &file : source_files) {
                if (file.size() <= old_name.size() || file.substr(0, old_name.size()) != old_name) {
                    // A file the commit info lists without the segment prefix cannot
                    // be renamed safely; skip it rather than write a stray file.
                    continue;
                }
                const std::string renamed = name + file.substr(old_name.size());
                if (auto copy = directory_.copy_from(source, file, renamed, context); !copy.has_value()) {
                    return copy;
                }
                copied.insert(renamed);
            }
            std::set<std::string> renamed_field_infos;
            for (const std::string &file : source_segment.field_infos_files) {
                renamed_field_infos.insert(name + file.substr(old_name.size()));
            }
            std::map<int32_t, std::set<std::string>> renamed_doc_values;
            for (const auto &[generation, files] : source_segment.doc_values_updates_files) {
                std::set<std::string> &renamed = renamed_doc_values[generation];
                for (const std::string &file : files) {
                    renamed.insert(name + file.substr(old_name.size()));
                }
            }
            target.field_infos_files = std::move(renamed_field_infos);
            target.doc_values_updates_files = std::move(renamed_doc_values);
            if (auto set = target.info.set_files(std::move(copied)); !set.has_value()) {
                return set;
            }
            if (target.info.use_compound_file) {
                // A compound file keeps its entries under the names they had when
                // it was packed, so the copy holds `_old.fnm` and the reader of the
                // renamed segment would look for `_new.fnm` inside it.  Unpacking
                // and repacking under the new name is what keeps both correct.
                if (auto unpacked = unpack_compound_segment(target, context); !unpacked.has_value()) {
                    return unpacked;
                }
            }
            // The segment info names the segment, so it has to be rewritten.  The
            // copied segment stays unpacked: repacking it would have to tell the
            // update generations (which stay outside) from the base ones, and an
            // unpacked segment is read exactly the same way.
            if (auto written = config_.codec().segment_info_format().write(directory_, target.info, context);
                !written.has_value()) {
                return written;
            }
            if (auto added = infos_.add(std::move(target)); !added.has_value()) {
                return added;
            }
        }

        documents_writer_->set_next_segment_counter(counter);
        infos_.counter = counter;
        return {};
    }

    [[nodiscard]] VoidResult commit() {
        if (auto prepared = prepare_commit(); !prepared.has_value()) {
            return prepared;
        }
        auto published = publish_commit();
        if (!published.has_value()) {
            return std::unexpected(std::move(published.error()));
        }
        return {};
    }

    /// Everything a commit does except making it visible.
    ///
    /// Port of `IndexWriter::prepare_commit` (`index/index_writer.rs`): the buffered
    /// documents become segments, the deletes and the doc values updates are applied
    /// and the merges run, but `segments_N` is not written, so a reader still sees the
    /// previous commit.  Nothing may be added between this and [`publish_commit`].
    [[nodiscard]] VoidResult prepare_commit() {
        if (commit_prepared_) {
            return {};
        }
        if (auto flushed = flush(); !flushed.has_value()) {
            return flushed;
        }
        const int64_t hidden_before = hidden_doc_count();
        if (auto began = begin_updates_packet(); !began.has_value()) {
            return began;
        }
        if (auto applied = apply_deletes(); !applied.has_value()) {
            return applied;
        }
        if (auto applied = apply_doc_values_updates(); !applied.has_value()) {
            return applied;
        }
        finish_updates_packet(hidden_doc_count() - hidden_before);
        documents_writer_->clear_delete_buffer();

        if (auto merged = maybe_merge(MergeTrigger::Commit); !merged.has_value()) {
            return merged;
        }

        // A live docs or doc values generation added files the `.si` on disk does not
        // list yet; the commit is the point where that becomes visible.
        if (auto refreshed = refresh_dirty_segment_info(); !refreshed.has_value()) {
            return refreshed;
        }

        commit_prepared_ = true;
        return {};
    }

    /// Whether a commit was prepared and is waiting to be published.
    [[nodiscard]] bool has_prepared_commit() const noexcept { return commit_prepared_; }

    /// Second phase of a two-phase commit: makes the prepared state visible.
    ///
    /// Everything else (the segments, the deletes, the doc values updates and the
    /// merges) already happened in the first phase; this only bumps the version and
    /// the generation, writes `segments_N`, syncs what the commit added and lets the
    /// deletion policy drop what nothing holds any more.
    [[nodiscard]] Result<int64_t> publish_commit() {
        if (!commit_prepared_) {
            return infos_.generation;
        }
        infos_.counter = documents_writer_->next_segment_counter();
        ++infos_.version;
        ++infos_.generation;
        if (auto written = infos_.write_to(directory_, io_context_default()); !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        // Durability: the files this commit added or rewrote have to reach stable
        // storage before the commit counts as published.  Only those are synced --
        // the files of an already committed segment did not change -- which is
        // also what lets an `NRTCachingDirectory` flush exactly the files it holds
        // in RAM (`Directory::sync` is the hook that copies them over).
        {
            std::set<std::string> known;
            for (const SegmentCommitInfo &segment : committed_infos_.segments) {
                known.insert(segment.info.name);
            }
            std::vector<std::string> to_sync;
            for (const SegmentCommitInfo &segment : infos_.segments) {
                const bool is_new = !known.contains(segment.info.name);
                const bool is_dirty = dirty_file_segments_.contains(segment.info.name);
                if (is_new || is_dirty) {
                    to_sync.insert(to_sync.end(), segment.info.files.begin(), segment.info.files.end());
                }
            }
            to_sync.push_back(infos_.generation_file_name());
            if (auto synced = directory_.sync(to_sync); !synced.has_value()) {
                return std::unexpected(std::move(synced.error()));
            }
            // The new `segments_N` (and the removals below) only become durable
            // once the directory entry itself is on stable storage.
            if (auto synced = directory_.sync_metadata(); !synced.has_value()) {
                return std::unexpected(std::move(synced.error()));
            }
        }
        committed_infos_ = infos_;
        // The commit point is published: protect its files, run the deletion
        // policy (which is what drops the previous commit when the policy says so)
        // and let the files nothing references any more go.  The files a merge
        // replaced are only reachable from the older commits, so they disappear
        // here when those commits do -- and stay when a policy keeps them.
        if (deleter_ != nullptr) {
            if (auto checked = deleter_->checkpoint(infos_, true, *config_.index_deletion_policy());
                !checked.has_value()) {
                return std::unexpected(std::move(checked.error()));
            }
        }
        orphan_files_.clear();
        commit_prepared_ = false;
        return infos_.generation;
    }

    [[nodiscard]] VoidResult rollback() {
        // A prepared commit is discarded as well: rolling back means "as if nothing
        // since the last commit happened", and a prepared state is exactly that.
        commit_prepared_ = false;
        // The pending deletions live in the pooled state; dropping it forgets them.
        reader_pool_->drop_all();
        updates_stream_->clear();
        current_packet_.reset();
        doc_values_updates_.clear();
        query_deletes_.clear();
        doc_deletes_.clear();
        dirty_file_segments_.clear();
        if (auto rolled = documents_writer_->rollback(); !rolled.has_value()) {
            return rolled;
        }
        // Segments written since the last commit are not referenced by any
        // `segments_N`: remove their files and forget about them.
        SegmentInfos uncommitted;
        std::set<std::string> keep = files_of(committed_infos_);
        for (SegmentCommitInfo &segment : infos_.segments) {
            bool committed = false;
            for (const SegmentCommitInfo &known : committed_infos_.segments) {
                if (known.info.name == segment.info.name) {
                    committed = true;
                    break;
                }
            }
            if (!committed) {
                for (const std::string &file : segment.info.files) {
                    if (keep.find(file) == keep.end()) {
                        (void)directory_.delete_file(file);
                    }
                }
            }
        }
        // The merge results are dropped, but the segments they replaced may
        // still be referenced by the committed state.
        for (const std::string &file : orphan_files_) {
            if (keep.find(file) == keep.end()) {
                (void)directory_.delete_file(file);
            }
        }
        orphan_files_.clear();
        infos_ = committed_infos_;
        documents_writer_->set_next_segment_counter(infos_.counter);
        // Whatever the dropped work left behind (files written but never
        // committed) is unreferenced now.
        if (deleter_ != nullptr) {
            if (auto refreshed = deleter_->refresh(); !refreshed.has_value()) {
                return refreshed;
            }
        }
        return {};
    }

    /// The `write.lock` this writer holds, or null when the directory has no
    /// locking (`ByteBuffersDirectory` hands out a `NoLock`).
    ///
    /// Declared before [`lock_wrapper_`] so it is initialised first and released
    /// last: the wrapper keeps a reference to it.
    std::unique_ptr<Lock> write_lock_;
    std::unique_ptr<LockValidatingDirectoryWrapper> lock_wrapper_;
    /// The directory the caller passed (`get_directory` answers with it);
    /// [`directory_`] is the lock-validating view of it.
    Directory &directory_orig_;
    Directory &directory_;
    /// Reference-counts the files of every live commit point and of the writer's
    /// current state, and runs the [`IndexDeletionPolicy`](:index_deletion_policy).
    std::shared_ptr<IndexFileDeleter> deleter_;

    /// The readers handed out by `get_reader`, so consecutive reads reuse them.

    std::unique_ptr<ReaderPool> reader_pool_;
    /// The packets of frozen deletes and updates, one per commit / near-real-time
    /// read that had something buffered.
    std::unique_ptr<BufferedUpdatesStream> updates_stream_ = std::make_unique<BufferedUpdatesStream>();
    /// The packet of the commit that is being applied, if any.
    std::shared_ptr<FrozenBufferedUpdates> current_packet_;
    IndexWriterConfig config_;
    SegmentInfos infos_;
    SegmentInfos committed_infos_;
    /// The doc values updates buffered since the last commit.
    FieldUpdatesBuffer doc_values_updates_;
    /// One buffered query delete: the query and the document count it was issued at.
    struct BufferedQueryDelete {
        std::shared_ptr<const Query> query;
        int64_t doc_upto{0};
    };
    /// One buffered doc values update of a single document.
    struct BufferedDocValueUpdate {
        std::string segment;
        int32_t doc{0};
        std::string field;
        DocValuesUpdateValue value;
    };
    /// One buffered delete of a single document, by segment name and local doc.
    struct BufferedDocDelete {
        std::string segment;
        int32_t doc{0};
        int64_t doc_upto{0};
    };
    std::vector<BufferedQueryDelete> query_deletes_;
    std::vector<BufferedDocDelete> doc_deletes_;
    std::vector<BufferedDocValueUpdate> doc_value_updates_;
    /// Segments that gained files (live docs, a doc values generation) since their
    /// `.si` was written; the commit refreshes the file list they record.
    std::set<std::string> dirty_file_segments_;
    std::unique_ptr<DocumentsWriter> documents_writer_;
    /// Merges the policy asked for and that have not run yet.
    std::vector<OneMerge> pending_merges_;
    /// Files of merged-away segments, deleted once the next commit is written.
    std::set<std::string> orphan_files_;
    std::atomic<bool> open_{true};
};

IndexWriter::IndexWriter(Directory &directory, IndexWriterConfig config, std::unique_ptr<Lock> write_lock)
    : impl_(std::make_unique<Impl>(directory, std::move(config), std::move(write_lock))) {
    // A merge that runs on a background thread takes the writer lock itself, so
    // the implementation needs the address of it.
    impl_->mutex = &mutex_;
}

IndexWriter::~IndexWriter() = default;

Result<std::unique_ptr<IndexWriter>> IndexWriter::open(Directory &directory, const IndexWriterConfig &config) {
    // One writer per index: the lock is taken before anything touches the
    // directory, exactly like the reference implementation, so a second writer
    // (or a second process) fails instead of writing into the same segments.
    auto write_lock = directory.obtain_lock(WRITE_LOCK_NAME);
    if (!write_lock.has_value()) {
        return std::unexpected(std::move(write_lock.error()));
    }

    auto generations = SegmentInfos::list_generations(directory);
    if (!generations.has_value()) {
        return std::unexpected(std::move(generations.error()));
    }

    // The open mode decides whether an existing index is expected or tolerated.
    const bool has_index = !generations.value().empty();
    if (config.open_mode() == OpenMode::Create && has_index) {
        return err(LuceneError::illegal_state("cannot create a new index: one already exists"));
    }
    if (config.open_mode() == OpenMode::Append && !has_index) {
        return err(LuceneError::index_not_found("cannot append: the directory holds no index"));
    }

    std::unique_ptr<IndexWriter> writer(new IndexWriter(directory, config, std::move(write_lock.value())));
    SegmentInfos infos;
    if (generations.value().empty()) {
        // A fresh index: the first commit writes `segments_1`.
        infos.version = 1;
        infos.counter = 0;
        infos.generation = 0;
        infos.index_created_version_major = config.index_created_version_major();
        infos.lucene_version = LUCENE_CURRENT_VERSION;
        const auto random = StringHelper::random_id();
        infos.id.assign(random.begin(), random.end());
    } else {
        auto read = SegmentInfos::read_latest(directory);
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        infos = std::move(read.value());
    }

    // Fail early when the configured index sort cannot order documents: the
    // flush path would otherwise fail on every segment.
    if (config.index_sort().has_value()) {
        for (const SortField &field : config.index_sort().value().fields()) {
            auto sorter = field.index_sorter();
            if (!sorter.has_value()) {
                auto text = field.to_string();
                return err(LuceneError::illegal_argument(
                        "cannot sort the index by " + (text.has_value() ? text.value() : std::string("?")) + ": " +
                        sorter.error().to_string()));
            }
        }
    }

    writer->impl_->infos_ = infos;
    writer->impl_->committed_infos_ = std::move(infos);

    // The file deleter loads every `segments_N` on disk, lets the deletion policy
    // drop the ones it does not want (that is where `on_init` runs) and removes
    // the files nothing references, which is also how the leftovers of an
    // interrupted writer get cleaned up.
    auto deleter = create_index_file_deleter(writer->impl_->directory_, *config.index_deletion_policy(),
                                             writer->impl_->infos_, get_default_info_stream(), has_index);
    if (!deleter.has_value()) {
        return std::unexpected(std::move(deleter.error()));
    }
    writer->impl_->deleter_ = std::move(deleter.value());
    if (writer->impl_->deleter_->starting_commit_deleted()) {
        // The policy dropped the commit the writer started from; the next commit
        // has to write a new one even when nothing changed.
        ++writer->impl_->infos_.version;
    }

    writer->impl_->documents_writer_ =
            std::make_unique<DocumentsWriter>(writer->impl_->directory_, config, writer->impl_->infos_.counter);
    writer->impl_->documents_writer_->set_max_buffers(config.max_indexing_buffers());
    if (config.flush_thread_count() > 0) {
        // A worker writes the buffers the indexing threads hand over, so indexing
        // never pays for the segment I/O.
        Impl *writer_impl = writer->impl_.get();
        writer->impl_->documents_writer_->start_flush_threads(
                [writer_impl](std::unique_ptr<DocumentsWriterPerThread> buffer) {
                    return writer_impl->write_and_publish(*buffer);
                },
                config.flush_thread_count());
    }
    return writer;
}

VoidResult IndexWriter::add_document(const Document &document) {
    if (!impl_->open_.load()) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // A single-buffered writer flushes inline, exactly like a writer that never had
    // a pool; with a pool, buffering takes only the pool's lock and a buffer that
    // filled up is written out by this thread without the writer lock, so several
    // threads index at the same time.
    if (impl_->documents_writer_->max_buffers() <= 1) {
        return impl_->documents_writer_->add_document(document);
    }
    auto full = impl_->documents_writer_->add_document_async(document);
    if (!full.has_value()) {
        return std::unexpected(std::move(full.error()));
    }
    if (full.value() == nullptr) {
        return {};
    }
    if (impl_->documents_writer_->enqueue_flush(full.value())) {
        return {};
    }
    return impl_->write_and_publish(*full.value());
}

VoidResult IndexWriter::add_documents(std::span<const Document *const> documents) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    for (const Document *document : documents) {
        if (document == nullptr) {
            return err(LuceneError::illegal_argument("a document to add is null"));
        }
        if (auto added = add_document(*document); !added.has_value()) {
            return added;
        }
    }
    return {};
}

bool IndexWriter::has_uncommitted_changes() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (impl_->infos_.version != impl_->committed_infos_.version) {
        return true;
    }
    if (impl_->documents_writer_->pending_doc_count() != 0) {
        return true;
    }
    if (!impl_->documents_writer_->buffered_deletes().empty() || impl_->documents_writer_->delete_all_pending()) {
        return true;
    }
    if (impl_->doc_values_updates_.any()) {
        return true;
    }
    if (!impl_->query_deletes_.empty() || !impl_->doc_deletes_.empty()) {
        return true;
    }
    return !impl_->pending_merges_.empty();
}

int32_t IndexWriter::num_buffered_documents() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->documents_writer_->pending_doc_count();
}

size_t IndexWriter::segment_count() const { return impl_->infos_.segments.size(); }

void IndexWriter::set_live_commit_data(std::map<std::string, std::string> user_data) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    impl_->infos_.user_data = std::move(user_data);
}

VoidResult IndexWriter::update_document(const Term &term, const Document &document) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered delete must not touch the document added right after it, so
    // it records how many documents the index held at this point.
    if (auto deleted = impl_->documents_writer_->delete_documents(term, impl_->total_docs());
        !deleted.has_value()) {
        return deleted;
    }
    return add_document(document);
}

VoidResult IndexWriter::update_document(std::shared_ptr<const Query> query, const Document &document) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The delete has to be buffered before the new document is added, so it cannot
    // reach the version it is about to write.
    if (auto deleted = delete_documents(std::move(query)); !deleted.has_value()) {
        return deleted;
    }
    return add_document(document);
}

VoidResult IndexWriter::soft_update_documents(const Term &term, std::span<const Document *const> documents) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    for (const Document *document : documents) {
        if (document == nullptr) {
            return err(LuceneError::illegal_argument("a document to soft update is null"));
        }
        if (auto updated = soft_update_document(term, *document); !updated.has_value()) {
            return updated;
        }
    }
    return {};
}

Result<std::vector<std::string>> IndexWriter::get_field_names() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    std::set<std::string> names;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        // A doc values update writes a new `.fnm` generation, which is the one that
        // records every field of the segment.
        const std::string suffix = segment.field_infos_gen == -1
                                           ? std::string{}
                                           : IndexFileNames::base36_encode(segment.field_infos_gen);
        // The generation the segment was written with can be inside the compound
        // file; an update generation is always next to it.
        std::shared_ptr<Directory> compound_directory;
        Directory *infos_directory = &impl_->directory_;
        if (segment.field_infos_gen == -1 && segment.info.use_compound_file) {
            const Codec *codec = impl_->config_.codec().name() == segment.info.codec_name
                                         ? &impl_->config_.codec()
                                         : CodecFormats::for_name(segment.info.codec_name);
            if (codec == nullptr || codec->compound_format() == nullptr) {
                return err(LuceneError::illegal_state("codec " + segment.info.codec_name +
                                                      " does not support compound files"));
            }
            auto opened = codec->compound_format()->get_compound_reader(impl_->directory_, segment.info);
            if (!opened.has_value()) {
                return std::unexpected(std::move(opened.error()));
            }
            compound_directory = std::move(opened.value());
            infos_directory = compound_directory.get();
        }
        auto infos = impl_->config_.codec().field_infos_format().read(*infos_directory, segment.info, suffix,
                                                                     io_context_default());
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        for (const FieldInfo &field : infos.value().fields()) {
            names.insert(field.name);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

VoidResult IndexWriter::maybe_merge() {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (auto flushed = impl_->flush(); !flushed.has_value()) {
        return flushed;
    }
    return impl_->maybe_merge(MergeTrigger::Explicit);
}

bool IndexWriter::has_pending_merges() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return !impl_->pending_merges_.empty();
}

const std::map<std::string, std::string> &IndexWriter::get_live_commit_data() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->infos_.user_data;
}

Directory &IndexWriter::get_directory() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->directory_orig_;
}

const IndexWriterConfig &IndexWriter::get_config() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->config_;
}

int64_t IndexWriter::flush_count() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->documents_writer_->flush_count();
}

size_t IndexWriter::pending_flush_count() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->documents_writer_->pending_flush_count();
}

int64_t IndexWriter::get_flushing_bytes() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->documents_writer_->bytes_used_in_ram();
}

Result<int64_t> IndexWriter::get_pending_num_docs() const { return num_docs(); }

VoidResult IndexWriter::soft_update_document(const Term &term, const Document &document) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // Lucene requires a soft deletes field, and so does this port: it is the field
    // the previous version of the document is stamped with.
    const std::optional<std::string> &field = impl_->config_.soft_deletes_field();
    if (!field.has_value()) {
        return err(LuceneError::illegal_argument(
                "soft deletes require IndexWriterConfig::set_soft_deletes_field"));
    }
    if (document.get_fields_with_name(*field).empty()) {
        return err(LuceneError::illegal_argument("the document of a soft update has to carry the soft deletes field \"" +
                                                *field + "\""));
    }
    if (auto deleted = impl_->documents_writer_->soft_delete_documents(term, impl_->total_docs());
        !deleted.has_value()) {
        return deleted;
    }
    return add_document(document);
}

VoidResult IndexWriter::update_documents(const Term &term, std::span<const Document *const> documents) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    for (const Document *document : documents) {
        if (document == nullptr) {
            return err(LuceneError::illegal_argument("a document to update is null"));
        }
    }
    // One buffered delete for the whole batch: the documents added below are newer
    // than its cut-off, so they survive it.
    if (auto deleted = impl_->documents_writer_->delete_documents(term, impl_->total_docs());
        !deleted.has_value()) {
        return deleted;
    }
    for (const Document *document : documents) {
        if (auto added = add_document(*document); !added.has_value()) {
            return added;
        }
    }
    return {};
}

VoidResult IndexWriter::delete_documents(const Term &term) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->documents_writer_->delete_documents(term, impl_->total_docs());
}

VoidResult IndexWriter::delete_documents(std::shared_ptr<const Query> query) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (query == nullptr) {
        return err(LuceneError::illegal_argument("a query to delete by is null"));
    }
    impl_->query_deletes_.push_back(Impl::BufferedQueryDelete{std::move(query), impl_->total_docs()});
    return {};
}

VoidResult IndexWriter::delete_documents(std::span<const std::shared_ptr<const Query>> queries) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    for (const std::shared_ptr<const Query> &query : queries) {
        if (auto deleted = delete_documents(query); !deleted.has_value()) {
            return deleted;
        }
    }
    return {};
}

VoidResult IndexWriter::update_doc_values(std::span<const DocValuesUpdate> updates) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    for (const DocValuesUpdate &update : updates) {
        auto term = update.buffered_term();
        if (!term.has_value()) {
            return err(term.error());
        }
        if (update.doc_values_type() == DocValuesType::Numeric) {
            auto numeric = update.numeric_update();
            if (!numeric.has_value()) {
                return err(numeric.error());
            }
            std::optional<int64_t> value;
            if (numeric.value()->has_value()) {
                value = numeric.value()->value().value();
            }
            if (auto added = update_numeric_doc_value(*term.value(), update.field(), value); !added.has_value()) {
                return added;
            }
        } else if (update.doc_values_type() == DocValuesType::Binary) {
            auto binary = update.binary_update();
            if (!binary.has_value()) {
                return err(binary.error());
            }
            std::optional<BytesRef> value;
            if (binary.value()->has_value()) {
                value = binary.value()->value().value();
            }
            if (auto added = update_binary_doc_value(*term.value(), update.field(), value); !added.has_value()) {
                return added;
            }
        } else {
            return err(LuceneError::illegal_argument(
                    "only numeric and binary doc values can be updated in place, field \"" + update.field() +
                    "\" is neither"));
        }
    }
    return {};
}

Result<bool> IndexWriter::try_update_doc_value(const IndexReader &reader, int32_t doc_id,
                                               std::span<const DocValuesUpdate> updates) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (doc_id < 0) {
        return err(LuceneError::illegal_argument("doc_id must not be negative, got " + std::to_string(doc_id)));
    }
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const LeafReaderContext &context : leaves.value()) {
        if (doc_id < context.doc_base() || doc_id >= context.doc_base() + context.reader()->max_doc().value_or(0)) {
            continue;
        }
        const auto *segment = dynamic_cast<const SegmentReader *>(context.reader());
        if (segment == nullptr) {
            return false;
        }
        const int32_t local = doc_id - context.doc_base();
        for (const DocValuesUpdate &update : updates) {
            impl_->doc_value_updates_.push_back(Impl::BufferedDocValueUpdate{
                    segment->segment_info().info.name, local, update.field(), update.sub_update()});
        }
        return true;
    }
    return false;
}

Result<bool> IndexWriter::try_delete_document(const IndexReader &reader, int32_t doc_id) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (doc_id < 0) {
        return err(LuceneError::illegal_argument("doc_id must not be negative, got " + std::to_string(doc_id)));
    }
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const LeafReaderContext &context : leaves.value()) {
        if (doc_id < context.doc_base() || doc_id >= context.doc_base() + context.reader()->max_doc().value_or(0)) {
            continue;
        }
        const auto *segment = dynamic_cast<const SegmentReader *>(context.reader());
        if (segment == nullptr) {
            // A leaf that is not a segment (a filtered or hand-built reader) cannot
            // be addressed by segment name.
            return false;
        }
        const int32_t local = doc_id - context.doc_base();
        impl_->doc_deletes_.push_back(Impl::BufferedDocDelete{segment->segment_info().info.name, local,
                                                             impl_->total_docs()});
        return true;
    }
    return false;
}

VoidResult IndexWriter::add_indexes_from_directory(Directory &source) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered documents become a segment first, so the counter the copied
    // segments pick up from is the one the writer would use next.
    if (auto flushed = impl_->flush(); !flushed.has_value()) {
        return flushed;
    }
    if (auto copied = impl_->add_indexes_from_directory(source); !copied.has_value()) {
        return copied;
    }
    return impl_->commit();
}

Result<int64_t> IndexWriter::add_indexes_from_codec_readers(const std::vector<std::shared_ptr<CodecReader>> &readers) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered documents become a segment first, so the new segment's name is
    // not handed out twice.
    if (auto flushed = impl_->flush(); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }
    auto added = impl_->add_indexes_from_codec_readers(readers);
    if (!added.has_value()) {
        return std::unexpected(std::move(added.error()));
    }
    if (auto committed = impl_->commit(); !committed.has_value()) {
        return std::unexpected(std::move(committed.error()));
    }
    return added.value();
}

VoidResult IndexWriter::delete_unused_files() {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    auto names = impl_->directory_.list_all();
    if (!names.has_value()) {
        return err(names.error());
    }

    // Everything the writer currently holds and everything the last commit named.
    std::set<std::string> used;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        used.insert(segment.info.files.begin(), segment.info.files.end());
        used.insert(segment.field_infos_files.begin(), segment.field_infos_files.end());
        for (const auto &[gen, files] : segment.doc_values_updates_files) {
            used.insert(files.begin(), files.end());
        }
    }
    for (const SegmentCommitInfo &segment : impl_->committed_infos_.segments) {
        used.insert(segment.info.files.begin(), segment.info.files.end());
    }
    // A file of a segment that is still around belongs to it even when the writer
    // does not name it (an interrupted generation, for instance).
    std::set<std::string> live_segments;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        live_segments.insert(segment.info.name);
    }
    for (const SegmentCommitInfo &segment : impl_->committed_infos_.segments) {
        live_segments.insert(segment.info.name);
    }

    for (const std::string &name : names.value()) {
        if (used.contains(name)) {
            continue;
        }
        if (is_commit_file_name(name) || name == "write.lock") {
            continue;
        }
        const std::string segment = segment_of(name);
        if (segment.empty() || live_segments.contains(segment)) {
            // Not a segment file, or it belongs to a segment that is still live.
            continue;
        }
        if (auto deleted = impl_->directory_.delete_file(name); !deleted.has_value()) {
            return deleted;
        }
    }
    return {};
}

VoidResult IndexWriter::update_numeric_doc_value(const Term &term, std::string field,
                                                 std::optional<int64_t> value) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    impl_->doc_values_updates_.add_numeric(term, std::move(field), impl_->total_docs(), value);
    return {};
}

VoidResult IndexWriter::update_binary_doc_value(const Term &term, std::string field,
                                                std::optional<BytesRef> value) {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    impl_->doc_values_updates_.add_binary(term, std::move(field), impl_->total_docs(), std::move(value));
    return {};
}

VoidResult IndexWriter::delete_all() {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->documents_writer_->delete_all();
}

VoidResult IndexWriter::flush() {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->flush();
}

VoidResult IndexWriter::wait_for_merges_unlocked() {
    const std::shared_ptr<MergeScheduler> &scheduler = impl_->config_.merge_scheduler();
    if (scheduler == nullptr) {
        return {};
    }
    return scheduler->wait_for_merges();
}

Result<int64_t> IndexWriter::prepare_commit() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return std::unexpected(std::move(waited.error()));
    }
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        if (!impl_->open_) {
            return err(LuceneError::already_closed("this IndexWriter is closed"));
        }
        if (auto prepared = impl_->prepare_commit(); !prepared.has_value()) {
            return std::unexpected(std::move(prepared.error()));
        }
    }
    // The merges the preparation queued have to be in the index before the second
    // phase publishes it.
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return std::unexpected(std::move(waited.error()));
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return static_cast<int64_t>(impl_->infos_.generation) + 1;
}

Result<int64_t> IndexWriter::commit_prepared() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return std::unexpected(std::move(waited.error()));
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->publish_commit();
}

bool IndexWriter::has_prepared_commit() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->has_prepared_commit();
}

VoidResult IndexWriter::commit() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    VoidResult result;
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        if (!impl_->open_) {
            return err(LuceneError::already_closed("this IndexWriter is closed"));
        }
        result = impl_->commit();
    }
    // The merges this commit queued run in the background; a committed index is
    // expected to be settled when `commit` returns, so they are waited for here
    // (the lock is released, otherwise their publish step would block).
    if (result.has_value()) {
        result = wait_for_merges_unlocked();
    }
    return result;
}

VoidResult IndexWriter::force_merge(int32_t max_num_segments) {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    VoidResult result;
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        if (!impl_->open_) {
            return err(LuceneError::already_closed("this IndexWriter is closed"));
        }
        // The merge has to see the buffered documents and deletes; the commit must
        // not queue merges of its own, they would race with the forced ones.
        impl_->suppress_merges_ = true;
        const VoidResult committed = impl_->commit();
        impl_->suppress_merges_ = false;
        if (!committed.has_value()) {
            return committed;
        }
        if (auto merged = impl_->force_merge(max_num_segments); !merged.has_value()) {
            return merged;
        }
    }
    // `force_merge` is synchronous: the merges a background scheduler started have
    // to be waited for (outside the lock, their publish step takes it) and the
    // result committed.
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    // Rewriting `segments_N` makes the merged segment the only visible one and
    // drops the files of the inputs.
    result = impl_->commit();
    return result;
}

VoidResult IndexWriter::force_merge_deletes() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        if (!impl_->open_) {
            return err(LuceneError::already_closed("this IndexWriter is closed"));
        }
        impl_->suppress_merges_ = true;
        const VoidResult committed = impl_->commit();
        impl_->suppress_merges_ = false;
        if (!committed.has_value()) {
            return committed;
        }
        if (auto merged = impl_->force_merge_deletes(); !merged.has_value()) {
            return merged;
        }
    }
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    return impl_->commit();
}

VoidResult IndexWriter::rollback() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->rollback();
}

VoidResult IndexWriter::close() {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return waited;
    }
    VoidResult result{};
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        if (!impl_->open_) {
            return {};
        }
        impl_->open_ = false;
        // No merge is proposed from here on, so nothing can be queued behind the
        // close and outlive the state a worker reads.
        impl_->closing_ = true;
        if (!impl_->config_.commit_on_close()) {
            // The configuration asks for the buffered changes to be dropped.
            result = impl_->rollback();
        } else {
            result = impl_->commit();
        }
    }
    // Merges that were already running finish while the writer still exists: they
    // take the writer lock themselves, so the wait has to happen outside it.
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value() && result.has_value()) {
        result = waited;
    }
    if (const std::shared_ptr<MergeScheduler> &scheduler = impl_->config_.merge_scheduler();
        scheduler != nullptr) {
        if (auto stopped = scheduler->close(); !stopped.has_value() && result.has_value()) {
            result = stopped;
        }
    }
    if (impl_->documents_writer_ != nullptr) {
        // The workers write into the writer, so they have to be gone before the
        // deleter and the lock are released.
        if (auto stopped = impl_->documents_writer_->stop_flush_threads();
            !stopped.has_value() && result.has_value()) {
            result = stopped;
        }
    }
    {
        const std::lock_guard<std::recursive_mutex> guard(mutex_);
        // Closing the deleter releases the last checkpoint (and with it the files of
        // commits nothing holds any more); releasing the lock lets another writer in.
        if (impl_->deleter_ != nullptr) {
            if (auto closed = impl_->deleter_->close(); !closed.has_value() && result.has_value()) {
                result = closed;
            }
            impl_->deleter_.reset();
        }
        if (impl_->write_lock_ != nullptr) {
            if (auto released = impl_->write_lock_->close(); !released.has_value() && result.has_value()) {
                result = released;
            }
        }
    }
    return result;
}

Result<std::shared_ptr<DirectoryReader>> IndexWriter::get_reader() { return get_reader(true, false); }

Result<std::shared_ptr<DirectoryReader>> IndexWriter::get_reader(bool apply_all_deletes, bool write_all_deletes) {
    if (auto waited = wait_for_merges_unlocked(); !waited.has_value()) {
        return std::unexpected(std::move(waited.error()));
    }
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    (void)write_all_deletes;
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered documents have to become a segment and the buffered deletes
    // have to reach the `.liv` files before a reader can see them.
    if (auto flushed = impl_->flush(); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }
    if (apply_all_deletes) {
        const int64_t hidden_before = impl_->hidden_doc_count();
        if (auto began = impl_->begin_updates_packet(); !began.has_value()) {
            return std::unexpected(std::move(began.error()));
        }
        if (auto applied = impl_->apply_deletes(); !applied.has_value()) {
            return std::unexpected(std::move(applied.error()));
        }
        if (auto applied = impl_->apply_doc_values_updates(); !applied.has_value()) {
            return std::unexpected(std::move(applied.error()));
        }
        impl_->finish_updates_packet(impl_->hidden_doc_count() - hidden_before);
    }

    // Reuse the readers of the previous near-real-time read; the pool reopens the
    // segments that changed (e.g. the ones a commit or a merge rewrote).
    // A commit that applied deletions replaces a segment, so drop the readers of
    // the segments the writer no longer holds.
    impl_->reader_pool_->retain_only(impl_->infos_);

    std::vector<std::shared_ptr<SegmentReader>> readers;
    readers.reserve(impl_->infos_.segments.size());
    for (SegmentCommitInfo &segment : impl_->infos_.segments) {
        auto reader = impl_->reader_pool_->get(segment, impl_->infos_.index_created_version_major,
                                               io_context_default());
        if (!reader.has_value()) {
            return std::unexpected(std::move(reader.error()));
        }
        readers.push_back(std::move(reader.value()));
    }
    return DirectoryReader::open_with_readers(impl_->directory_, impl_->infos_, std::move(readers));
}

DocStats IndexWriter::get_doc_stats() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    DocStats stats;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        stats.num_docs += segment.doc_count();
        stats.max_doc += segment.max_doc();
    }
    stats.num_docs += impl_->documents_writer_->pending_flushed_doc_count();
    stats.num_docs += impl_->documents_writer_->pending_doc_count();
    stats.max_doc += impl_->documents_writer_->pending_flushed_doc_count();
    stats.max_doc += impl_->documents_writer_->pending_doc_count();
    return stats;
}

Result<int64_t> IndexWriter::num_docs() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    int64_t count = 0;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        count += segment.doc_count();
    }
    // The flushed segments of the queue are committed documents in the making, so
    // they count as pending even though they are not in `infos_` yet.
    count += impl_->documents_writer_->pending_flushed_doc_count();
    count += impl_->documents_writer_->pending_doc_count();
    return count;
}

Result<int64_t> IndexWriter::max_doc() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    int64_t count = 0;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        count += segment.max_doc();
    }
    count += impl_->documents_writer_->pending_doc_count();
    return count;
}

Result<bool> IndexWriter::has_deletions() const {
    const std::lock_guard<std::recursive_mutex> guard(mutex_);
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        if (segment.total_del_count() > 0) {
            return true;
        }
    }
    return false;
}

const SegmentInfos &IndexWriter::segment_infos() const { return impl_->infos_; }

bool IndexWriter::is_open() const noexcept { return impl_->open_; }

} // namespace lucene
