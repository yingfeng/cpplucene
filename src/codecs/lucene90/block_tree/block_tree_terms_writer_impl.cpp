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

module lucene_core:block_tree_terms_writer.impl;

import :block_tree_terms_writer;

import :array_util;
import :block_term_state;
import :block_tree_msb_vlong;
import :block_tree_terms_constants;
import :bytes_ref;
import :bytes_ref_builder;
import :codec_util;
import :compression_algorithm;
import :core_helper;
import :data_output;
import :debug;
import :error;
import :field_infos;
import :fixed_bit_set;
import :fst;
import :fst_builder;
import :fst_byte_sequence_outputs;
import :fst_outputs;
import :fst_store;
import :fst_util;
import :growable_byte_array_data_output;
import :index_output;
import :index_types;
import :ints_ref;
import :lowercase_ascii_compression;
import :lz4;
import :norms_producer;
import :postings_enum;
import :postings_writer_base;
import :segment_info;
import :segment_write_state;
import :string_helper;
import :terms_enum;
import :types;

import std;

namespace lucene {

namespace {

/// Packs a block's file pointer and its two flags into the FST output of the
/// term index.  `encode_output` of the reference implementation.
int64_t encode_output(int64_t fp, bool has_terms, bool is_floor) {
    debug_check(fp < (int64_t{1} << 62), "a block file pointer must fit in 62 bits");
    return (fp << 2) | (has_terms ? static_cast<int64_t>(OUTPUT_FLAG_HAS_TERMS) : 0) |
           (is_floor ? static_cast<int64_t>(OUTPUT_FLAG_IS_FLOOR) : 0);
}

/// `true` when `bytes[start_offset, end_offset)` is `value` throughout.
Result<bool> all_equal(const std::vector<uint8_t> &bytes, size_t start_offset, size_t end_offset, uint8_t value) {
    if (start_offset > end_offset || end_offset > bytes.size()) {
        return err(LuceneError::array_index_out_of_bounds(
                std::format("start_offset: {}, end_offset: {}, length: {}", start_offset, end_offset, bytes.size())));
    }
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(start_offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(end_offset),
                       [value](uint8_t byte) { return byte == value; });
}

/// `write_bytes_ref`: a vint length followed by that many bytes.
VoidResult write_bytes_ref(DataOutput &out, const BytesRef &bytes) {
    if (auto written = out.write_vint(static_cast<int32_t>(bytes.length())); !written) {
        return written;
    }
    return out.write_bytes(bytes.as_slice());
}

/// Writes the statistics of one block, run-length encoding the terms that occur
/// in exactly one document.  Port of `StatsWriter`.
class StatsWriter {
public:
    StatsWriter(DataOutput &out, bool has_freqs) : out_(&out), has_freqs_(has_freqs) {}

    VoidResult add(int32_t df, int64_t ttf) {
        if (df == 1 && (!has_freqs_ || ttf == 1)) {
            ++singleton_count_;
            return {};
        }
        if (auto finished = finish(); !finished) {
            return finished;
        }
        if (auto written = out_->write_vint(df << 1); !written) {
            return written;
        }
        if (!has_freqs_) {
            return {};
        }
        return out_->write_vlong(ttf - static_cast<int64_t>(df));
    }

    VoidResult finish() {
        if (singleton_count_ > 0) {
            if (auto written = out_->write_vint(((singleton_count_ - 1) << 1) | 1); !written) {
                return written;
            }
            singleton_count_ = 0;
        }
        return {};
    }

private:
    DataOutput *out_;
    bool has_freqs_;
    int32_t singleton_count_{0};
};

/// One compiled block-tree index: a byte-sequence FST over the block prefixes
/// of one block (or of one set of floor blocks), plus the bytes it was built
/// from.
///
/// **Deviation:** the reference implementation keeps an `FST<ByteSequenceOutputs,
/// DataOutputEnum<DummyIndexOutput>>` that can write itself back out; this
/// port's [`FST`] reads from an owned byte store, so the bytes are held next to
/// the FST for [`save`] and [`num_bytes`].
class CompiledIndex {
public:
    [[nodiscard]] static Result<CompiledIndex> create(FstBuilder::BuildResult result) {
        CompiledIndex index;
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(result.data));
        index.bytes_ = bytes;
        index.metadata_ = std::move(result.metadata);

        const OnHeapFstStore store(bytes);
        auto reader = store.make_reader();
        if (reader == nullptr) {
            return err(LuceneError::illegal_state("FST metadata and store are some, should not return None"));
        }
        index.fst_ = std::make_unique<FST>(index.metadata_, std::move(reader));
        return index;
    }

    CompiledIndex() = default;
    ~CompiledIndex() = default;
    CompiledIndex(CompiledIndex &&) noexcept = default;
    CompiledIndex &operator=(CompiledIndex &&) noexcept = default;

    [[nodiscard]] int64_t num_bytes() const noexcept { return metadata_.num_bytes; }
    [[nodiscard]] const std::optional<std::any> &empty_output() const noexcept { return metadata_.empty_output; }
    [[nodiscard]] const FST &fst() const noexcept { return *fst_; }

    /// Writes the FST metadata to `meta_out` and the FST bytes to `out`
    /// (`FST::save` of the reference implementation).
    [[nodiscard]] VoidResult save(DataOutput &meta_out, DataOutput &out) const {
        if (auto written = metadata_.save(meta_out); !written) {
            return written;
        }
        return out.write_bytes(*bytes_);
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_{};
    FSTMetadata metadata_{};
    std::unique_ptr<FST> fst_{};
};

/// A term that has been written to the postings files and is waiting for its
/// block to be written.  Port of `PendingTerm`.
struct PendingTerm {
    /// The term bytes; the offset is always 0.
    BytesRef term;
    /// Term metadata, encoded by the postings writer when the block is written.
    TermStateEnum state;
};

/// A block that has been written to `.tim` and now waits for its parent block.
/// Port of `PendingBlock`.
struct PendingBlock {
    /// Longest prefix shared by all entries; its length is the block's prefix
    /// length.
    BytesRef prefix;
    /// File pointer of the block in `.tim`.
    int64_t fp{0};
    /// The compiled index of this block, set once its sub index is merged.
    std::unique_ptr<CompiledIndex> index{};
    /// Sub-indexes of the entries (set while the block is still pending).
    std::vector<CompiledIndex> sub_indices{};
    bool has_terms{false};
    bool is_floor{false};
    int32_t floor_lead_byte{-1};

    /// Builds the FST index that maps each of the block's prefixes to its
    /// output, merging in the sub-indexes of the pending blocks.
    ///
    /// The returned block is `blocks[0]`; the other blocks are compiled into it
    /// and dropped, exactly like the reference implementation's `drain(..)`.
    [[nodiscard]] static Result<PendingBlock> compile_index(std::vector<PendingBlock> &blocks,
                                                            GrowableByteArrayDataOutput &scratch_bytes,
                                                            IntsRefBuilder &scratch_ints_ref);
};

/// Emits every accepted key of `fst`, in lexicographic order.
///
/// **Deviation:** the reference implementation uses `BytesRefFSTEnum`, which needs the whole
/// `fst_enum` stack; the writer only needs "walk everything once", so a depth
/// first walk over the arcs covers it.
[[nodiscard]] VoidResult walk_fst(const FST &fst, const FstOutputs &outputs, std::vector<int32_t> &key,
                                  const Arc &follow, const std::any &follow_output,
                                  const std::function<VoidResult(std::span<const int32_t>, const std::any &)> &visit) {
    Arc arc;
    if (auto read = fst.read_first_target_arc(follow, arc, fst.reader()); !read) {
        return read;
    }
    while (true) {
        if (arc.label() == END_LABEL) {
            // The virtual arc inserted for a final node: its output completes
            // the key.
            const std::any key_output = outputs.add(follow_output, arc.output());
            if (auto visited = visit(key, key_output); !visited) {
                return visited;
            }
            if (arc.is_last()) {
                return {};
            }
            if (auto next = fst.read_next_arc(arc, fst.reader()); !next) {
                return next;
            }
            continue;
        }

        debug_check(arc.is_final() || arc.target() > 0, "a non-final node without arcs accepts no key");
        if (arc.is_final() || arc.target() > 0) {
            key.push_back(arc.label());
            const std::any child_output = outputs.add(follow_output, arc.output());
            if (auto walked = walk_fst(fst, outputs, key, arc, child_output, visit); !walked) {
                return walked;
            }
            key.pop_back();
        }

        if (arc.is_last()) {
            return {};
        }
        if (auto next = fst.read_next_arc(arc, fst.reader()); !next) {
            return next;
        }
    }
}

/// Re-adds every entry of `sub_index` to `fst_compiler` (`PendingBlock::append`).
VoidResult append_index(const CompiledIndex &sub_index, FstBuilder &fst_compiler, IntsRefBuilder &scratch_ints_ref) {
    const FstByteSequenceOutputs &outputs = FstByteSequenceOutputs::singleton();
    std::vector<int32_t> key;

    Arc start_arc;
    sub_index.fst().get_first_arc(start_arc);
    return walk_fst(sub_index.fst(), outputs, key, start_arc, outputs.get_no_output(),
                    [&fst_compiler, &scratch_ints_ref](std::span<const int32_t> input,
                                                       const std::any &output) -> VoidResult {
                        if (auto copied = scratch_ints_ref.copy_ints(input, 0, input.size()); !copied) {
                            return copied;
                        }
                        auto added = fst_compiler.add(
                                std::span<const int32_t>(scratch_ints_ref.ints().data(), scratch_ints_ref.length()),
                                std::any(output));
                        if (!added) {
                            return err(std::move(added.error()));
                        }
                        return {};
                    });
}

Result<PendingBlock> PendingBlock::compile_index(std::vector<PendingBlock> &blocks,
                                                 GrowableByteArrayDataOutput &scratch_bytes,
                                                 IntsRefBuilder &scratch_ints_ref) {
    if (blocks.empty()) {
        return err(LuceneError::illegal_state("compiled block list is empty"));
    }
    debug_check((blocks.size() > 1 && blocks[0].is_floor) || (!blocks[0].is_floor && blocks.size() == 1),
                "a block set is one block or several floor blocks");
    debug_check(scratch_bytes.size() == 0, "the index scratch output must be empty");

    const bool is_floor = blocks[0].is_floor;
    const int64_t fp = blocks[0].fp;
    const size_t prefix_len = blocks[0].prefix.length();

    // The output of the first block; it doubles as the key's value in the FST.
    if (auto written = write_msb_vlong(encode_output(fp, blocks[0].has_terms, is_floor), scratch_bytes); !written) {
        return err(std::move(written.error()));
    }

    if (is_floor) {
        const size_t following_blocks = blocks.size() - 1;
        if (following_blocks > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("too many floor blocks"));
        }
        if (auto written = scratch_bytes.write_vint(static_cast<int32_t>(following_blocks)); !written) {
            return err(std::move(written.error()));
        }
        for (size_t i = 1; i < blocks.size(); ++i) {
            const PendingBlock &block = blocks[i];
            debug_check(block.floor_lead_byte != -1, "a floor block knows its lead byte");
            if (auto written = scratch_bytes.write_byte(static_cast<uint8_t>(block.floor_lead_byte)); !written) {
                return err(std::move(written.error()));
            }
            debug_check(block.fp > fp, "floor block file pointers grow");
            const int64_t delta_fp = ((block.fp - fp) << 1) | (block.has_terms ? 1 : 0);
            if (auto written = scratch_bytes.write_vlong(delta_fp); !written) {
                return err(std::move(written.error()));
            }
        }
    }

    // The reference implementation derives the page size of its paged FST writer from the estimated
    // index size; this port builds the FST in one contiguous buffer, so only the
    // prefix length matters here.

    const FstByteSequenceOutputs &outputs = FstByteSequenceOutputs::singleton();
    FstBuilder fst_compiler(outputs, InputType::kByte1);

    BytesRef first_key(scratch_bytes.bytes());
    scratch_bytes.reset();
    debug_check(!first_key.as_slice().empty(), "the encoded block output is never empty");

    if (auto to_ints = FstUtil::to_ints_ref(blocks[0].prefix, scratch_ints_ref); !to_ints) {
        return err(std::move(to_ints.error()));
    }
    auto added = fst_compiler.add(std::span<const int32_t>(scratch_ints_ref.ints().data(), scratch_ints_ref.length()),
                                  std::any(std::move(first_key)));
    if (!added) {
        return err(std::move(added.error()));
    }

    for (PendingBlock &block : blocks) {
        std::vector<CompiledIndex> sub_indices = std::move(block.sub_indices);
        block.sub_indices.clear();
        for (const CompiledIndex &sub_index : sub_indices) {
            if (auto appended = append_index(sub_index, fst_compiler, scratch_ints_ref); !appended) {
                return err(std::move(appended.error()));
            }
        }
    }

    auto compiled = fst_compiler.finish();
    if (!compiled) {
        return err(std::move(compiled.error()));
    }
    auto index = CompiledIndex::create(std::move(compiled.value()));
    if (!index) {
        return err(std::move(index.error()));
    }
    blocks[0].index = std::make_unique<CompiledIndex>(std::move(index.value()));
    debug_check(blocks[0].sub_indices.empty(), "the compiled block keeps no sub index");
    (void)prefix_len;

    return std::move(blocks[0]);
}

/// Writes the terms of one field and collects its `.tmd` record.
/// Port of `TermsWriter`.
class TermsWriter {
public:
    [[nodiscard]] static Result<std::unique_ptr<TermsWriter>> create(const FieldInfo &field_info, int32_t max_doc,
                                                                     PostingsWriterBase &postings_writer,
                                                                     int32_t min_items_in_block,
                                                                     int32_t max_items_in_block, int32_t version,
                                                                     IndexOutput &terms_out) {
        debug_check(field_info.index_options != IndexOptions::None, "a field without terms is not written");

        postings_writer.set_field(field_info);

        auto writer = std::unique_ptr<TermsWriter>(new TermsWriter());
        writer->field_info_ = &field_info;
        writer->max_doc_ = max_doc;
        writer->docs_seen_ = FixedBitSet(static_cast<size_t>(max_doc < 0 ? 0 : max_doc));
        writer->prefix_starts_.assign(8, 0);
        writer->pending_.reserve(static_cast<size_t>(std::min(max_items_in_block, BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE)));
        writer->min_items_in_block_ = min_items_in_block;
        writer->max_items_in_block_ = max_items_in_block;
        writer->version_ = version;
        writer->postings_writer_ = &postings_writer;
        writer->terms_out_ = &terms_out;
        return writer;
    }

    /// Writes the postings of `text` and pushes the term onto the pending stack.
    ///
    /// Returns the [`PostingsEnum`] the postings writer may reuse for the next
    /// term (the reference implementation's `Option<PE>` return value).
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> write(const BytesRef &text, TermsEnum &terms_enum,
                                                              const NormsProducer *norms,
                                                              std::unique_ptr<PostingsEnum> postings_enum) {
        auto written = postings_writer_->write_term(text, terms_enum, docs_seen_, norms, std::move(postings_enum));
        if (!written) {
            return err(std::move(written.error()));
        }
        WriteTermResult result = std::move(written.value());
        if (!result.state.has_value()) {
            return std::move(result.postings);
        }

        const TermStateEnum &state = result.state.value();
        int64_t total_term_freq = 0;
        int32_t doc_freq = 0;
        if (const auto *block = std::get_if<BlockTermState>(&state.value()); block != nullptr) {
            debug_check(block->doc_freq != 0, "a pending term has at least one document");
            total_term_freq = block->total_term_freq;
            doc_freq = block->doc_freq;
        } else if (const auto *int_state = std::get_if<IntBlockTermState>(&state.value()); int_state != nullptr) {
            debug_check(int_state->base.doc_freq != 0, "a pending term has at least one document");
            total_term_freq = int_state->base.total_term_freq;
            doc_freq = int_state->base.doc_freq;
        } else {
            return err(LuceneError::illegal_state("should be Block or Int"));
        }
        debug_check(field_info_->index_options == IndexOptions::Docs || total_term_freq >= static_cast<int64_t>(doc_freq),
                    "the total term frequency cannot be smaller than the document frequency");

        if (auto pushed = push_term(text); !pushed) {
            return err(std::move(pushed.error()));
        }

        auto owned = BytesRef::deep_copy_of(text);
        if (!owned) {
            return err(std::move(owned.error()));
        }
        if (!first_pending_term_.has_value()) {
            first_pending_term_ = owned.value();
        }
        last_pending_term_ = owned.value();

        sum_doc_freq_ += static_cast<int64_t>(doc_freq);
        sum_total_term_freq_ += total_term_freq;
        ++num_terms_;

        pending_.push_back(PendingTerm{std::move(owned.value()), state});
        return std::move(result.postings);
    }

    /// Writes the remaining pending entries as blocks, then the field's `.tmd`
    /// record into `fields`.
    [[nodiscard]] VoidResult finish(std::vector<GrowableByteArrayDataOutput> &fields, IndexOutput &index_out) {
        if (num_terms_ == 0) {
            debug_check(sum_total_term_freq_ == 0 ||
                                (field_info_->index_options == IndexOptions::Docs && sum_total_term_freq_ == -1),
                        "an empty field has no total term frequency");
            debug_check(sum_doc_freq_ == 0, "an empty field has no document frequency");
            debug_check(docs_seen_.cardinality() == 0, "an empty field saw no documents");
            return {};
        }

        // The root block must hold every remaining entry, so push the empty term
        // twice: the first one flushes the whole stack, the second one is a
        // no-op that keeps the loop in `push_term` from writing a partial block.
        const BytesRef empty_term{};
        if (auto pushed = push_term(empty_term); !pushed) {
            return pushed;
        }
        if (auto pushed = push_term(empty_term); !pushed) {
            return pushed;
        }

        if (auto written = write_blocks(0, pending_.size()); !written) {
            return written;
        }

        debug_check(pending_.size() == 1, "the root block replaces every pending entry");
        if (pending_.size() != 1 || std::get_if<PendingBlock>(&pending_[0]) == nullptr) {
            return err(LuceneError::illegal_state("expected the final root block"));
        }
        PendingBlock root = std::get<PendingBlock>(std::move(pending_[0]));
        pending_.pop_back();
        debug_check(root.prefix.length() == 0, "the root block has an empty prefix");

        if (root.index == nullptr) {
            return err(LuceneError::illegal_state("root terms index is missing"));
        }
        if (!root.index->empty_output().has_value()) {
            return err(LuceneError::illegal_state("root terms index has no empty output"));
        }
        const auto *root_code = std::any_cast<BytesRef>(&root.index->empty_output().value());
        if (root_code == nullptr) {
            return err(LuceneError::illegal_state("the root output is not a byte sequence"));
        }
        if (!first_pending_term_.has_value() || !last_pending_term_.has_value()) {
            return err(LuceneError::illegal_state("first and last pending terms are missing"));
        }

        GrowableByteArrayDataOutput meta_out;
        if (auto written = meta_out.write_vint(field_info_->number); !written) {
            return written;
        }
        if (auto written = meta_out.write_vlong(num_terms_); !written) {
            return written;
        }
        if (root_code->length() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("the root code does not fit in a vint"));
        }
        if (auto written = meta_out.write_vint(static_cast<int32_t>(root_code->length())); !written) {
            return written;
        }
        if (auto written = meta_out.write_bytes(root_code->as_slice(), 0, root_code->length()); !written) {
            return written;
        }
        if (field_info_->index_options != IndexOptions::Docs) {
            if (auto written = meta_out.write_vlong(sum_total_term_freq_); !written) {
                return written;
            }
        }
        if (auto written = meta_out.write_vlong(sum_doc_freq_); !written) {
            return written;
        }
        if (docs_seen_.cardinality() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("the document count does not fit in a vint"));
        }
        if (auto written = meta_out.write_vint(static_cast<int32_t>(docs_seen_.cardinality())); !written) {
            return written;
        }
        if (auto written = write_bytes_ref(meta_out, first_pending_term_.value()); !written) {
            return written;
        }
        if (auto written = write_bytes_ref(meta_out, last_pending_term_.value()); !written) {
            return written;
        }
        auto index_start_fp = index_out.get_file_pointer();
        if (!index_start_fp) {
            return err(std::move(index_start_fp.error()));
        }
        if (auto written = meta_out.write_vlong(static_cast<int64_t>(index_start_fp.value())); !written) {
            return written;
        }
        if (auto saved = root.index->save(meta_out, index_out); !saved) {
            return saved;
        }

        fields.push_back(std::move(meta_out));
        return {};
    }

    /// Writes the last `count` pending entries as one block (or as a set of
    /// floor blocks) and replaces them with the resulting block.
    [[nodiscard]] VoidResult write_blocks(size_t prefix_length, size_t count) {
        debug_check(count > 0, "write_blocks needs at least one entry");
        // The root block better write all remaining pending entries.
        debug_check(prefix_length > 0 || count == pending_.size(), "only the root block writes everything");

        int32_t last_suffix_lead_label = -1;
        // True if we saw at least one term in this block (a block that only
        // points to sub-blocks is recorded as such in the terms index, so that
        // the reader can avoid seeking to it when looking for a term).
        bool has_terms = false;
        bool has_sub_blocks = false;

        const size_t start = pending_.size() - count;
        const size_t end = pending_.size();
        size_t next_block_start = start;
        int32_t next_floor_lead_label = -1;

        for (size_t i = start; i < end; ++i) {
            int32_t suffix_lead_label = -1;
            bool is_term = false;
            if (const auto *term = std::get_if<PendingTerm>(&pending_[i]); term != nullptr) {
                if (term->term.length() == prefix_length) {
                    // Suffix is 0: the term *is* the prefix, so it has an empty
                    // suffix in this block.
                    debug_check(last_suffix_lead_label == -1, "the empty suffix is unique in a block");
                } else {
                    suffix_lead_label = term->term.as_slice()[prefix_length];
                }
                is_term = true;
            } else {
                const PendingBlock &block = std::get<PendingBlock>(pending_[i]);
                debug_check(block.prefix.length() > prefix_length, "a sub block extends the prefix");
                suffix_lead_label = block.prefix.as_slice()[prefix_length];
            }

            if (suffix_lead_label != last_suffix_lead_label) {
                const size_t items_in_block = i - next_block_start;
                if (items_in_block >= static_cast<size_t>(min_items_in_block_) &&
                    end - next_block_start > static_cast<size_t>(max_items_in_block_)) {
                    // Too many entries for one block, so it is broken into
                    // "floor" blocks, each recording the leading label of the
                    // suffix of its first term.
                    const bool is_floor = items_in_block < count;
                    auto block = write_block(prefix_length, is_floor, next_floor_lead_label, next_block_start, i,
                                             has_terms, has_sub_blocks);
                    if (!block) {
                        return err(std::move(block.error()));
                    }
                    new_blocks_.push_back(std::move(block.value()));

                    has_terms = false;
                    has_sub_blocks = false;
                    next_floor_lead_label = suffix_lead_label;
                    next_block_start = i;
                }
                last_suffix_lead_label = suffix_lead_label;
            }
            if (is_term) {
                has_terms = true;
            } else {
                has_sub_blocks = true;
            }
        }

        if (next_block_start < end) {
            const size_t items_in_block = end - next_block_start;
            const bool is_floor = items_in_block < count;
            auto block = write_block(prefix_length, is_floor, next_floor_lead_label, next_block_start, end, has_terms,
                                     has_sub_blocks);
            if (!block) {
                return err(std::move(block.error()));
            }
            new_blocks_.push_back(std::move(block.value()));
        }

        debug_check(!new_blocks_.empty(), "a block set is never empty");
        debug_check(new_blocks_[0].is_floor || new_blocks_.size() == 1, "only floor blocks come in groups");

        auto compiled = PendingBlock::compile_index(new_blocks_, scratch_bytes_, scratch_ints_ref_);
        if (!compiled) {
            return err(std::move(compiled.error()));
        }
        // The reference implementation drains the whole vector: the remaining blocks are folded into
        // the first one and discarded.
        new_blocks_.clear();

        const size_t remove_start = pending_.size() - count;
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(remove_start), pending_.end());
        pending_.push_back(std::move(compiled.value()));
        return {};
    }

    /// Writes the entries `[start, end)` of the pending stack as one block.
    ///
    /// When `is_floor` is true the entries shared a prefix with more than
    /// `max_items_in_block` siblings, so they were split into floor blocks and
    /// `floor_lead_label` is the leading label of this one's first suffix.
    [[nodiscard]] Result<PendingBlock> write_block(size_t prefix_length, bool is_floor, int32_t floor_lead_label,
                                                   size_t start, size_t end, bool has_terms, bool has_sub_blocks) {
        debug_check(end > start, "a block holds at least one entry");

        auto start_fp = terms_out_->get_file_pointer();
        if (!start_fp) {
            return err(std::move(start_fp.error()));
        }
        const int64_t block_fp = static_cast<int64_t>(start_fp.value());
        const bool has_floor_lead = is_floor && floor_lead_label != -1;

        std::vector<uint8_t> prefix_bytes;
        prefix_bytes.reserve(prefix_length + (has_floor_lead ? 1 : 0));
        const std::vector<uint8_t> &last_term_bytes = last_term_.get_bytes_ref().bytes();
        debug_check(prefix_length <= last_term_.length(), "the prefix is a prefix of the last term");
        prefix_bytes.insert(prefix_bytes.end(), last_term_bytes.begin(),
                            last_term_bytes.begin() + static_cast<std::ptrdiff_t>(prefix_length));
        if (has_floor_lead) {
            // Reserve the slot the floor lead byte is written into below.
            prefix_bytes.push_back(0);
        }
        BytesRef prefix(std::move(prefix_bytes));
        prefix.set_length(prefix_length);

        const size_t num_entries = end - start;
        int32_t code = static_cast<int32_t>(num_entries << 1);
        if (end == pending_.size()) {
            code |= 1;
        }
        if (auto written = terms_out_->write_vint(code); !written) {
            return err(std::move(written.error()));
        }

        const bool is_leaf_block = !has_sub_blocks;
        std::vector<CompiledIndex> sub_indices;
        if (has_sub_blocks) {
            sub_indices.reserve(num_entries);
        }
        bool absolute = true;
        const bool has_freqs = field_info_->index_options != IndexOptions::Docs;

        if (is_leaf_block) {
            StatsWriter stats_writer(stats_writer_, has_freqs);
            for (size_t i = start; i < end; ++i) {
                auto *term = std::get_if<PendingTerm>(&pending_[i]);
                if (term == nullptr) {
                    return err(LuceneError::illegal_state("Expected PendingTerm"));
                }
                debug_check(StringHelper::starts_with(term->term.as_slice(), prefix.as_slice()),
                            "the term starts with the block prefix");
                const size_t suffix = term->term.length() - prefix_length;

                if (auto written = suffix_lengths_writer_.write_vint(static_cast<int32_t>(suffix)); !written) {
                    return err(std::move(written.error()));
                }
                if (auto appended = suffix_writer_.append(term->term.as_slice().subspan(prefix_length)); !appended) {
                    return err(std::move(appended.error()));
                }
                debug_check(floor_lead_label == -1 ||
                                    (term->term.as_slice()[prefix_length] >= static_cast<uint8_t>(floor_lead_label)),
                            "the term is in this floor block");

                if (auto added = add_stats(stats_writer, term->state); !added) {
                    return err(std::move(added.error()));
                }
                if (auto encoded = postings_writer_->encode_term(meta_writer_, *field_info_, term->state, absolute);
                    !encoded) {
                    return err(std::move(encoded.error()));
                }
                absolute = false;
            }
            if (auto finished = stats_writer.finish(); !finished) {
                return err(std::move(finished.error()));
            }
        } else {
            StatsWriter stats_writer(stats_writer_, has_freqs);
            for (size_t i = start; i < end; ++i) {
                if (auto *term = std::get_if<PendingTerm>(&pending_[i]); term != nullptr) {
                    debug_check(StringHelper::starts_with(term->term.as_slice(), prefix.as_slice()),
                                "the term starts with the block prefix");
                    const size_t suffix_len = term->term.length() - prefix_length;

                    if (auto written = suffix_lengths_writer_.write_vint(static_cast<int32_t>(suffix_len << 1));
                        !written) {
                        return err(std::move(written.error()));
                    }
                    if (auto appended = suffix_writer_.append(term->term.as_slice().subspan(prefix_length));
                        !appended) {
                        return err(std::move(appended.error()));
                    }
                    if (auto added = add_stats(stats_writer, term->state); !added) {
                        return err(std::move(added.error()));
                    }
                    if (auto encoded = postings_writer_->encode_term(meta_writer_, *field_info_, term->state, absolute);
                        !encoded) {
                        return err(std::move(encoded.error()));
                    }
                    absolute = false;
                } else {
                    auto &block = std::get<PendingBlock>(pending_[i]);
                    debug_check(StringHelper::starts_with(block.prefix.as_slice(), prefix.as_slice()),
                                "the sub block starts with the block prefix");
                    const size_t suffix = block.prefix.length() - prefix_length;
                    debug_check(suffix > 0, "an entry of a non leaf block has a non empty suffix");

                    // write block suffix
                    if (auto written = suffix_lengths_writer_.write_vint(static_cast<int32_t>((suffix << 1) | 1));
                        !written) {
                        return err(std::move(written.error()));
                    }
                    if (auto appended = suffix_writer_.append(block.prefix.as_slice().subspan(prefix_length));
                        !appended) {
                        return err(std::move(appended.error()));
                    }

                    debug_check(floor_lead_label == -1 ||
                                        (block.prefix.as_slice()[prefix_length] >=
                                         static_cast<uint8_t>(floor_lead_label)),
                                "the sub block is in this floor block");
                    debug_check(block.fp < block_fp, "sub blocks are written before their parent");

                    if (auto written = suffix_lengths_writer_.write_vlong(block_fp - block.fp); !written) {
                        return err(std::move(written.error()));
                    }
                    if (block.index == nullptr) {
                        return err(LuceneError::illegal_state("floor block index is missing"));
                    }
                    sub_indices.push_back(std::move(*block.index));
                    block.index.reset();
                }
            }
            if (auto finished = stats_writer.finish(); !finished) {
                return err(std::move(finished.error()));
            }
            debug_check(!sub_indices.empty(), "a non leaf block has sub blocks");
        }

        // Write the suffix byte blob to the terms dictionary output, either
        // uncompressed, compressed with LZ4 or with LowercaseAsciiCompression.
        CompressionAlgorithm compression_alg = CompressionAlgorithm::NoCompression;
        const size_t suffix_len = suffix_writer_.length();
        // If there are 2 suffix bytes or less per term, compression is not worth
        // it, and the same holds for blocks with a 1 or 2 byte prefix: those are
        // fully visited by a fuzzy query with at most two edits.
        if (suffix_len > 2 * num_entries && prefix_length > 2) {
            if (suffix_len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                return err(LuceneError::illegal_argument("the suffix block is too large to compress"));
            }
            const auto suffix_len_i32 = static_cast<int32_t>(suffix_len);
            if (suffix_len > 6 * num_entries) {
                if (compression_hash_table_ == nullptr) {
                    compression_hash_table_ = std::make_unique<HighCompressionHashTable>();
                }
                if (auto compressed = LZ4::compress(suffix_writer_.get_bytes_ref().bytes(), 0, suffix_len_i32,
                                                    spare_writer_, *compression_hash_table_);
                    !compressed) {
                    return err(std::move(compressed.error()));
                }
                if (spare_writer_.size() < suffix_len - (suffix_len >> 2)) {
                    compression_alg = CompressionAlgorithm::Lz4;
                }
            }

            if (compression_alg == CompressionAlgorithm::NoCompression) {
                spare_writer_.reset();
                if (auto grown = ArrayUtil::grow_no_copy(spare_bytes_, suffix_len, [] { return uint8_t{0}; });
                    !grown) {
                    return err(std::move(grown.error()));
                }
                auto compressed = LowercaseAsciiCompression::compress(suffix_writer_.get_bytes_ref().bytes(), suffix_len,
                                                                      spare_bytes_, spare_writer_);
                if (!compressed) {
                    return err(std::move(compressed.error()));
                }
                if (compressed.value()) {
                    compression_alg = CompressionAlgorithm::LowercaseAscii;
                }
            }
        }

        uint64_t token = static_cast<uint64_t>(suffix_len) << 3;
        if (is_leaf_block) {
            token |= 0x04;
        }
        token |= compression_algorithm_code(compression_alg);
        if (auto written = terms_out_->write_vlong(static_cast<int64_t>(token)); !written) {
            return err(std::move(written.error()));
        }

        if (compression_alg == CompressionAlgorithm::NoCompression) {
            if (auto written = terms_out_->write_bytes(suffix_writer_.get_bytes_ref().as_slice()); !written) {
                return err(std::move(written.error()));
            }
        } else if (auto written = spare_writer_.copy_to(*terms_out_); !written) {
            return err(std::move(written.error()));
        }
        suffix_writer_.set_length(0);
        spare_writer_.reset();

        // suffix lengths
        const size_t num_suffix_bytes = suffix_lengths_writer_.size();
        if (num_suffix_bytes == 0) {
            return err(LuceneError::illegal_state("a block writes at least one suffix length"));
        }
        if (auto grown = ArrayUtil::grow_no_copy(spare_bytes_, num_suffix_bytes, [] { return uint8_t{0}; }); !grown) {
            return err(std::move(grown.error()));
        }
        {
            ByteArrayDataOutput data_output(std::span<uint8_t>(spare_bytes_.data(), num_suffix_bytes));
            if (auto copied = suffix_lengths_writer_.copy_to(data_output); !copied) {
                return err(std::move(copied.error()));
            }
        }
        suffix_lengths_writer_.reset();

        auto equal = all_equal(spare_bytes_, 1, num_suffix_bytes, spare_bytes_[0]);
        if (!equal) {
            return err(std::move(equal.error()));
        }
        if (num_suffix_bytes > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / 2) {
            return err(LuceneError::illegal_argument("the suffix length block does not fit in a vint"));
        }
        if (equal.value()) {
            if (auto written = terms_out_->write_vint(static_cast<int32_t>((num_suffix_bytes << 1) | 1)); !written) {
                return err(std::move(written.error()));
            }
            if (auto written = terms_out_->write_byte(spare_bytes_[0]); !written) {
                return err(std::move(written.error()));
            }
        } else {
            if (auto written = terms_out_->write_vint(static_cast<int32_t>(num_suffix_bytes << 1)); !written) {
                return err(std::move(written.error()));
            }
            if (auto written = terms_out_->write_bytes(
                        std::span<const uint8_t>(spare_bytes_.data(), num_suffix_bytes));
                !written) {
                return err(std::move(written.error()));
            }
        }

        // stats
        if (stats_writer_.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("the statistics block does not fit in a vint"));
        }
        if (auto written = terms_out_->write_vint(static_cast<int32_t>(stats_writer_.size())); !written) {
            return err(std::move(written.error()));
        }
        if (auto copied = stats_writer_.copy_to(*terms_out_); !copied) {
            return err(std::move(copied.error()));
        }
        stats_writer_.reset();

        // meta
        if (meta_writer_.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("the metadata block does not fit in a vint"));
        }
        if (auto written = terms_out_->write_vint(static_cast<int32_t>(meta_writer_.size())); !written) {
            return err(std::move(written.error()));
        }
        if (auto copied = meta_writer_.copy_to(*terms_out_); !copied) {
            return err(std::move(copied.error()));
        }
        meta_writer_.reset();

        if (has_floor_lead) {
            prefix.bytes()[prefix.length()] = static_cast<uint8_t>(floor_lead_label);
            prefix.set_length(prefix.length() + 1);
        }

        PendingBlock block;
        block.prefix = std::move(prefix);
        block.fp = block_fp;
        block.has_terms = has_terms;
        block.is_floor = is_floor;
        block.floor_lead_byte = floor_lead_label;
        block.sub_indices = std::move(sub_indices);
        return block;
    }

private:
    /// Records the statistics of `state` in `stats_writer`.
    static VoidResult add_stats(StatsWriter &stats_writer, const TermStateEnum &state) {
        if (const auto *block = std::get_if<BlockTermState>(&state.value()); block != nullptr) {
            return stats_writer.add(block->doc_freq, block->total_term_freq);
        }
        if (const auto *int_state = std::get_if<IntBlockTermState>(&state.value()); int_state != nullptr) {
            return stats_writer.add(int_state->base.doc_freq, int_state->base.total_term_freq);
        }
        return err(LuceneError::illegal_state("should be Block or Int"));
    }

    /// Records `text` as the last term seen and closes the blocks the new term
    /// makes complete.
    VoidResult push_term(const BytesRef &text) {
        const BytesRef &last_bytes = last_term_.get_bytes_ref();
        const std::span<const uint8_t> last_span(last_term_.get_bytes_ref().bytes().data(), last_term_.length());
        int32_t mismatch = CoreHelper::mismatch_u8(last_span, text.as_slice());
        if (mismatch == -1) {
            debug_check(last_term_.length() == 0, "two identical terms in one field");
            mismatch = 0;
        }
        const auto prefix_length = static_cast<size_t>(mismatch);

        for (size_t i = last_bytes.length(); i-- > prefix_length;) {
            const int32_t prefix_top_size = static_cast<int32_t>(pending_.size()) - prefix_starts_[i];
            if (prefix_top_size >= min_items_in_block_) {
                if (auto written = write_blocks(i + 1, static_cast<size_t>(prefix_top_size)); !written) {
                    return written;
                }
                prefix_starts_[i] -= prefix_top_size - 1;
            }
        }

        // Records the index into `pending_` where the current prefix at that
        // length started: when a new block begins at `prefix_length`, the terms
        // sharing the longer prefixes all start there.
        const size_t old_length = prefix_starts_.size();
        const int32_t pending_count = static_cast<int32_t>(pending_.size());
        if (old_length < text.length()) {
            if (auto grown = ArrayUtil::grow_with(prefix_starts_, text.length(),
                                                 [pending_count] { return pending_count; });
                !grown) {
                return grown;
            }
        }
        for (size_t i = prefix_length; i < std::min(text.length(), old_length); ++i) {
            prefix_starts_[i] = pending_count;
        }

        return last_term_.copy_bytes(text);
    }

    const FieldInfo *field_info_{nullptr};
    int64_t num_terms_{0};
    int32_t max_doc_{0};
    /// Documents that have at least one term of this field.
    FixedBitSet docs_seen_{};
    int64_t sum_total_term_freq_{0};
    int64_t sum_doc_freq_{0};
    BytesRefBuilder last_term_{};
    std::vector<int32_t> prefix_starts_{};
    std::vector<std::variant<PendingTerm, PendingBlock>> pending_{};
    /// Reused in [`write_blocks`].
    std::vector<PendingBlock> new_blocks_{};
    GrowableByteArrayDataOutput suffix_lengths_writer_{};
    BytesRefBuilder suffix_writer_{};
    GrowableByteArrayDataOutput stats_writer_{};
    GrowableByteArrayDataOutput meta_writer_{};
    GrowableByteArrayDataOutput spare_writer_{};
    std::vector<uint8_t> spare_bytes_{};
    std::unique_ptr<HighCompressionHashTable> compression_hash_table_{};
    int32_t min_items_in_block_{0};
    int32_t max_items_in_block_{0};
    GrowableByteArrayDataOutput scratch_bytes_{};
    IntsRefBuilder scratch_ints_ref_{};
    int32_t version_{BLOCK_TREE_VERSION_CURRENT};
    std::optional<BytesRef> first_pending_term_{};
    std::optional<BytesRef> last_pending_term_{};
    IndexOutput *terms_out_{nullptr};
    PostingsWriterBase *postings_writer_{nullptr};
};

} // namespace

VoidResult BlockTreeTermsWriter::validate_settings(int32_t min_items_in_block, int32_t max_items_in_block) {
    if (min_items_in_block <= 1) {
        return err(LuceneError::illegal_argument(
                std::format("min_items_in_block must be >= 2; got {}", min_items_in_block)));
    }
    if (min_items_in_block > max_items_in_block) {
        return err(LuceneError::illegal_argument(std::format(
                "max_items_in_block must be >= min_items_in_block; got max_items_in_block={}, min_items_in_block={}",
                max_items_in_block, min_items_in_block)));
    }
    if (2 * (min_items_in_block - 1) > max_items_in_block) {
        return err(LuceneError::illegal_argument(std::format(
                "max_items_in_block must be at least 2*(min_items_in_block-1); got max_items_in_block={}, "
                "min_items_in_block={}",
                max_items_in_block, min_items_in_block)));
    }
    return {};
}

Result<std::unique_ptr<BlockTreeTermsWriter>> BlockTreeTermsWriter::create(const SegmentWriteState &state,
                                                                           std::unique_ptr<PostingsWriterBase> postings_writer,
                                                                           int32_t min_items_in_block,
                                                                           int32_t max_items_in_block,
                                                                           const SegmentInfo &segment_info) {
    if (auto valid = validate_settings(min_items_in_block, max_items_in_block); !valid) {
        return err(std::move(valid.error()));
    }
    if (state.field_infos == nullptr) {
        return err(LuceneError::illegal_argument("the segment write state has no field infos"));
    }
    if (postings_writer == nullptr) {
        return err(LuceneError::illegal_argument("postings writer is missing"));
    }

    std::unique_ptr<BlockTreeTermsWriter> writer(new BlockTreeTermsWriter());
    writer->max_doc_ = segment_info.max_doc;
    writer->min_items_in_block_ = min_items_in_block;
    writer->max_items_in_block_ = max_items_in_block;
    writer->version_ = BLOCK_TREE_VERSION_CURRENT;
    writer->field_infos_ = state.field_infos;

    const std::span<const uint8_t> id(segment_info.id);

    // ---- .tim ----
    const std::string terms_name = segment_file_name(segment_info.name, state.segment_suffix, TERMS_EXTENSION);
    auto terms_out = state.directory.create_output(terms_name, state.context);
    if (!terms_out) {
        return err(std::move(terms_out.error()));
    }
    writer->terms_out_ = std::move(terms_out.value());
    if (auto header = CodecUtil::write_index_header(*writer->terms_out_, TERMS_CODEC_NAME, writer->version_, id,
                                                    state.segment_suffix);
        !header) {
        writer->close_quietly();
        return err(std::move(header.error()));
    }

    // ---- .tip ----
    const std::string index_name = segment_file_name(segment_info.name, state.segment_suffix, TERMS_INDEX_EXTENSION);
    auto index_out = state.directory.create_output(index_name, state.context);
    if (!index_out) {
        writer->close_quietly();
        return err(std::move(index_out.error()));
    }
    writer->index_out_ = std::move(index_out.value());
    if (auto header = CodecUtil::write_index_header(*writer->index_out_, TERMS_INDEX_CODEC_NAME, writer->version_, id,
                                                    state.segment_suffix);
        !header) {
        writer->close_quietly();
        return err(std::move(header.error()));
    }

    // ---- .tmd ----
    const std::string meta_name = segment_file_name(segment_info.name, state.segment_suffix, TERMS_META_EXTENSION);
    auto meta_out = state.directory.create_output(meta_name, state.context);
    if (!meta_out) {
        writer->close_quietly();
        return err(std::move(meta_out.error()));
    }
    writer->meta_out_ = std::move(meta_out.value());
    if (auto header = CodecUtil::write_index_header(*writer->meta_out_, TERMS_META_CODEC_NAME, writer->version_, id,
                                                    state.segment_suffix);
        !header) {
        writer->close_quietly();
        return err(std::move(header.error()));
    }

    if (auto initialized = postings_writer->init(*writer->meta_out_, state, segment_info); !initialized) {
        writer->close_quietly();
        return err(std::move(initialized.error()));
    }
    writer->postings_writer_ = std::move(postings_writer);
    return writer;
}

Result<std::unique_ptr<BlockTreeTermsWriter>> BlockTreeTermsWriter::create_default(
        const SegmentWriteState &state, std::unique_ptr<PostingsWriterBase> postings_writer,
        const SegmentInfo &segment_info) {
    return create(state, std::move(postings_writer), DEFAULT_MIN_BLOCK_SIZE, DEFAULT_MAX_BLOCK_SIZE, segment_info);
}

BlockTreeTermsWriter::~BlockTreeTermsWriter() {
    if (!closed_) {
        if (auto closed = close(); !closed) {
            std::cerr << "Error closing BlockTreeTermsWriter: " << closed.error().to_string() << '\n';
        }
    }
}

void BlockTreeTermsWriter::close_quietly() noexcept {
    closed_ = true;
    // Every close is attempted even when an earlier one fails; the first
    // failure is what the caller already knows about, so the rest is dropped.
    if (postings_writer_ != nullptr) {
        (void)postings_writer_->close();
    }
    if (meta_out_ != nullptr) {
        (void)meta_out_->close();
    }
    if (terms_out_ != nullptr) {
        (void)terms_out_->close();
    }
    if (index_out_ != nullptr) {
        (void)index_out_->close();
    }
}

VoidResult BlockTreeTermsWriter::write(const SegmentWriteState &state, const SegmentInfo &segment_info, Fields &fields,
                                       NormsProducer *norms) {
    (void)state;
    (void)segment_info;
    if (closed_) {
        return err(LuceneError::already_closed("the terms writer is already closed"));
    }

    auto field_names = fields.iterator();
    if (!field_names) {
        return err(std::move(field_names.error()));
    }

    std::string last_field;
    bool has_last_field = false;
    while (true) {
        auto name = (*field_names)->next();
        if (!name) {
            return err(std::move(name.error()));
        }
        if (name.value() == nullptr) {
            break;
        }
        const std::string &field = *name.value();
        if (has_last_field) {
            debug_check(last_field < field, "fields must be written in lexicographic order");
        }
        last_field = field;
        has_last_field = true;

        auto terms = fields.terms(field);
        if (!terms) {
            return err(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            continue;
        }
        auto terms_enum = terms.value()->iterator();
        if (!terms_enum) {
            return err(std::move(terms_enum.error()));
        }

        const FieldInfo *field_info = field_infos_->by_name(field);
        if (field_info == nullptr) {
            return err(LuceneError::illegal_state(std::format("Missing fields:{}", field)));
        }

        auto terms_writer = TermsWriter::create(*field_info, max_doc_, *postings_writer_, min_items_in_block_,
                                                max_items_in_block_, version_, *terms_out_);
        if (!terms_writer) {
            return err(std::move(terms_writer.error()));
        }

        std::unique_ptr<PostingsEnum> reuse;
        while (true) {
            auto term = (*terms_enum)->next();
            if (!term) {
                return err(std::move(term.error()));
            }
            if (term.value() == nullptr) {
                break;
            }
            auto written = (*terms_writer)
                                   ->write(*term.value(), **terms_enum, norms, std::move(reuse));
            if (!written) {
                return err(std::move(written.error()));
            }
            reuse = std::move(written.value());
        }

        if (auto finished = (*terms_writer)->finish(fields_, *index_out_); !finished) {
            return finished;
        }
    }
    return {};
}

VoidResult BlockTreeTermsWriter::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;

    auto write_metadata = [this]() -> VoidResult {
        if (fields_.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return err(LuceneError::illegal_argument("too many fields for one terms dictionary"));
        }
        if (auto written = meta_out_->write_vint(static_cast<int32_t>(fields_.size())); !written) {
            return written;
        }
        for (const GrowableByteArrayDataOutput &field_meta : fields_) {
            if (auto written = field_meta.copy_to(*meta_out_); !written) {
                return written;
            }
        }
        if (auto written = CodecUtil::write_footer(*index_out_); !written) {
            return written;
        }
        auto index_fp = index_out_->get_file_pointer();
        if (!index_fp) {
            return err(std::move(index_fp.error()));
        }
        if (auto written = meta_out_->write_long(static_cast<int64_t>(index_fp.value())); !written) {
            return written;
        }
        if (auto written = CodecUtil::write_footer(*terms_out_); !written) {
            return written;
        }
        auto terms_fp = terms_out_->get_file_pointer();
        if (!terms_fp) {
            return err(std::move(terms_fp.error()));
        }
        if (auto written = meta_out_->write_long(static_cast<int64_t>(terms_fp.value())); !written) {
            return written;
        }
        return CodecUtil::write_footer(*meta_out_);
    };

    if (auto written = write_metadata(); !written) {
        close_quietly();
        return written;
    }

    // The reference implementation's order: metadata, terms, index, then the postings writer.
    std::optional<LuceneError> first_error;
    auto close_one = [&first_error](VoidResult result) {
        if (!result && !first_error.has_value()) {
            first_error = std::move(result.error());
        }
    };
    close_one(meta_out_->close());
    close_one(terms_out_->close());
    close_one(index_out_->close());
    if (postings_writer_ != nullptr) {
        close_one(postings_writer_->close());
    }
    if (first_error.has_value()) {
        return std::unexpected(std::move(first_error.value()));
    }
    return {};
}

} // namespace lucene
