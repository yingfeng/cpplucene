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

module lucene_core:block_tree_intersect_terms_enum.impl;

import :block_tree_intersect_terms_enum;

import :block_term_state;
import :block_tree_output_accumulator;
import :block_tree_segment_terms_enum;
import :block_tree_terms_constants;
import :bytes_ref;
import :bytes_ref_builder;
import :compiled_automaton;
import :compression_algorithm;
import :data_input;
import :error;
import :field_infos;
import :fst;
import :impacts_enum;
import :index_input;
import :index_types;
import :postings_enum;
import :postings_reader_base;
import :string_helper;
import :terms;
import :terms_enum;
import :transition;
import :types;

import std;

namespace lucene {

namespace {

/// The FST outputs are type erased (`FstOutput`); a missing slot is NO_OUTPUT.
const BytesRef &arc_output(const FstOutput &slot) {
    const auto *output = slot.as_ptr<BytesRef>();
    return output != nullptr ? *output : no_output();
}

/// Lexicographic comparison of two byte strings.
int32_t compare_bytes(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    const auto order = std::lexicographical_compare_three_way(left.begin(), left.end(), right.begin(), right.end());
    if (order < 0) {
        return -1;
    }
    return order > 0 ? 1 : 0;
}

} // namespace

// ===========================================================================
// IntersectTermsEnumFrame
// ===========================================================================

Result<IntersectTermsEnumFrame> IntersectTermsEnumFrame::create(size_t ord, FieldReader &fr) {
    if (fr.parent() == nullptr) {
        return err(LuceneError::illegal_state("block-tree terms reader is missing"));
    }
    IntersectTermsEnumFrame frame;
    auto state = fr.parent()->postings_reader().new_term_state();
    if (!state) {
        return std::unexpected(std::move(state.error()));
    }
    frame.term_state = std::move(state.value());
    auto block = frame.term_state.get_block_term_state_mut();
    if (!block) {
        return std::unexpected(std::move(block.error()));
    }
    (*block)->total_term_freq = -1;

    frame.ord = ord;

    // The reference implementation pre-sizes the buffers the same way; they grow on demand.
    frame.suffix_bytes.assign(128, 0);
    frame.suffix_length_bytes.assign(32, 0);
    frame.stats_bytes.assign(64, 0);
    frame.metadata_bytes.assign(32, 0);
    frame.floor_bytes.assign(32, 0);
    frame.reseat_readers();
    return frame;
}

void IntersectTermsEnumFrame::reseat_readers() {
    suffixes_reader.reset_with_range(std::span<const uint8_t>(suffix_bytes), 0, 0);
    suffix_lengths_reader.reset_with_range(std::span<const uint8_t>(suffix_length_bytes), 0, 0);
    stats_reader.reset_with_range(std::span<const uint8_t>(stats_bytes), 0, 0);
    bytes_reader.reset_with_range(std::span<const uint8_t>(metadata_bytes), 0, 0);
    floor_data_reader.reset_with_range(std::span<const uint8_t>(floor_bytes), 0, 0);
}

VoidResult IntersectTermsEnumFrame::set_state(AutomatonEnum &automaton, IntersectTermsEnumFrame &frame,
                                             int32_t state) {
    frame.state = state;
    frame.transition_index = 0;
    auto count = automaton.get_num_transitions_with_state(state);
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    frame.transition_count = count.value();
    if (frame.transition_count != 0) {
        auto init = automaton.init_transition(state, frame.transition);
        if (!init) {
            return std::unexpected(std::move(init.error()));
        }
        auto first = automaton.get_next_transition(frame.transition);
        if (!first) {
            return std::unexpected(std::move(first.error()));
        }
    } else {
        // Must set min to -1 so the "label < min" check never falsely triggers,
        // and max to -1 so we immediately step to the next transition and pop
        // this frame.
        frame.transition.min = -1;
        frame.transition.max = -1;
    }
    return {};
}

VoidResult IntersectTermsEnumFrame::load_next_floor_block(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    while (true) {
        auto delta = frame.floor_data_reader.read_vlong();
        if (!delta) {
            return std::unexpected(std::move(delta.error()));
        }
        frame.fp = static_cast<size_t>(static_cast<int64_t>(frame.fp_orig) +
                                       static_cast<int64_t>(static_cast<uint64_t>(delta.value()) >> 1));
        frame.num_follow_floor_blocks -= 1;
        if (frame.num_follow_floor_blocks != 0) {
            auto label = frame.floor_data_reader.read_byte();
            if (!label) {
                return std::unexpected(std::move(label.error()));
            }
            frame.next_floor_label = static_cast<int32_t>(label.value());
        } else {
            frame.next_floor_label = 256;
        }

        if (frame.num_follow_floor_blocks != 0 && frame.next_floor_label <= frame.transition.min) {
            continue;
        }
        break;
    }
    return load(frame_idx, std::nullopt, ite);
}

VoidResult IntersectTermsEnumFrame::load_from_frame_index_data(const BytesRef &frame_index_data, size_t frame_idx,
                                                              IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    // The reader borrows its bytes, so the frame owns a copy.
    if (frame.floor_bytes.size() < frame_index_data.length()) {
        frame.floor_bytes.resize(frame_index_data.length(), 0);
    }
    std::copy_n(frame_index_data.as_slice().begin(), static_cast<std::ptrdiff_t>(frame_index_data.length()),
                frame.floor_bytes.begin());
    frame.floor_data_reader.reset_with_range(std::span<const uint8_t>(frame.floor_bytes), 0,
                                             frame_index_data.length());

    auto code = ite.fr().read_vlong_output(frame.floor_data_reader);
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    return load(frame_idx, code.value(), ite);
}

VoidResult IntersectTermsEnumFrame::load_from_output_accumulator(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    ite.output_accumulator().prepare_read();
    auto code = ite.fr().read_vlong_output(ite.output_accumulator());
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    if (auto copied = ite.output_accumulator().set_floor_data(frame.floor_bytes); !copied) {
        return copied;
    }
    frame.floor_data_reader.reset_with_range(std::span<const uint8_t>(frame.floor_bytes), 0, frame.floor_bytes.size());
    return load(frame_idx, code.value(), ite);
}

VoidResult IntersectTermsEnumFrame::load(size_t frame_idx, std::optional<int64_t> block_code,
                                         IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];

    if (block_code.has_value()) {
        // This block is the first one of a possible sequence of floor blocks
        // corresponding to a single seek point from the FST terms index.
        if ((block_code.value() & static_cast<int64_t>(OUTPUT_FLAG_IS_FLOOR)) != 0) {
            auto follow_blocks = frame.floor_data_reader.read_vint();
            if (!follow_blocks) {
                return std::unexpected(std::move(follow_blocks.error()));
            }
            frame.num_follow_floor_blocks = follow_blocks.value();
            auto label = frame.floor_data_reader.read_byte();
            if (!label) {
                return std::unexpected(std::move(label.error()));
            }
            frame.next_floor_label = static_cast<int32_t>(label.value());

            // If the current state is not accepting and has transitions, process
            // the first block in case it has an empty suffix.
            auto accepted = ite.automaton().is_accept(frame.state);
            if (!accepted) {
                return std::unexpected(std::move(accepted.error()));
            }
            if (!accepted.value() && frame.transition_count != 0) {
                while (frame.num_follow_floor_blocks != 0 && frame.next_floor_label <= frame.transition.min) {
                    auto delta = frame.floor_data_reader.read_vlong();
                    if (!delta) {
                        return std::unexpected(std::move(delta.error()));
                    }
                    frame.fp = static_cast<size_t>(static_cast<int64_t>(frame.fp_orig) +
                                                   static_cast<int64_t>(static_cast<uint64_t>(delta.value()) >> 1));
                    frame.num_follow_floor_blocks -= 1;
                    if (frame.num_follow_floor_blocks != 0) {
                        auto next_label = frame.floor_data_reader.read_byte();
                        if (!next_label) {
                            return std::unexpected(std::move(next_label.error()));
                        }
                        frame.next_floor_label = static_cast<int32_t>(next_label.value());
                    } else {
                        frame.next_floor_label = 256;
                    }
                }
            }
        }
    }

    IndexInput *input = ite.input().get();
    if (input == nullptr) {
        return err(LuceneError::illegal_state("block-tree terms input is not initialized"));
    }
    if (auto seeked = input->seek(frame.fp); !seeked) {
        return std::unexpected(std::move(seeked.error()));
    }

    auto code = input->read_vint();
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    frame.ent_count = static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
    frame.is_last_in_floor = (code.value() & 1) != 0;

    // Term suffixes.
    auto code_l = input->read_vlong();
    if (!code_l) {
        return std::unexpected(std::move(code_l.error()));
    }
    frame.is_leaf_block = (code_l.value() & 0x04) != 0;
    const size_t num_suffix_bytes = static_cast<size_t>(static_cast<uint64_t>(code_l.value()) >> 3);
    if (frame.suffix_bytes.size() < num_suffix_bytes) {
        frame.suffix_bytes.resize(num_suffix_bytes, 0);
    }
    auto algorithm = compression_algorithm_by_code(static_cast<uint8_t>(code_l.value() & 0x03));
    if (!algorithm) {
        return err(LuceneError::corrupt_index(
                std::format("{} (resource={})", algorithm.error().message(), input->to_string())));
    }
    if (auto read = read_compressed(algorithm.value(), *input, std::span<uint8_t>(frame.suffix_bytes),
                                    static_cast<int32_t>(num_suffix_bytes));
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    frame.suffixes_reader.reset_with_range(std::span<const uint8_t>(frame.suffix_bytes), 0, num_suffix_bytes);

    auto suffix_length_size = input->read_vint();
    if (!suffix_length_size) {
        return std::unexpected(std::move(suffix_length_size.error()));
    }
    if (suffix_length_size.value() < 0) {
        return err(LuceneError::corrupt_index("negative suffix length block size"));
    }
    const bool all_equal = (suffix_length_size.value() & 1) != 0;
    const size_t num_suffix_length_bytes = static_cast<size_t>(suffix_length_size.value()) >> 1;
    if (frame.suffix_length_bytes.size() < num_suffix_length_bytes) {
        frame.suffix_length_bytes.resize(num_suffix_length_bytes, 0);
    }
    if (all_equal) {
        auto fill = input->read_byte();
        if (!fill) {
            return std::unexpected(std::move(fill.error()));
        }
        std::fill_n(frame.suffix_length_bytes.begin(), static_cast<std::ptrdiff_t>(num_suffix_length_bytes),
                    fill.value());
    } else {
        const std::span<uint8_t> target(frame.suffix_length_bytes);
        if (auto read = input->read_bytes(target.first(num_suffix_length_bytes)); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    frame.suffix_lengths_reader.reset_with_range(std::span<const uint8_t>(frame.suffix_length_bytes), 0,
                                                 num_suffix_length_bytes);

    // Statistics.
    auto stats_size = input->read_vint();
    if (!stats_size) {
        return std::unexpected(std::move(stats_size.error()));
    }
    if (stats_size.value() < 0) {
        return err(LuceneError::corrupt_index("negative statistics block size"));
    }
    const size_t num_stats_bytes = static_cast<size_t>(stats_size.value());
    if (frame.stats_bytes.size() < num_stats_bytes) {
        frame.stats_bytes.resize(num_stats_bytes, 0);
    }
    {
        const std::span<uint8_t> target(frame.stats_bytes);
        if (auto read = input->read_bytes(target.first(num_stats_bytes)); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    frame.stats_reader.reset_with_range(std::span<const uint8_t>(frame.stats_bytes), 0, num_stats_bytes);
    frame.stats_singleton_run_length = 0;
    frame.meta_data_upto = 0;

    auto block_state = frame.term_state.get_block_term_state_mut();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    (*block_state)->term_block_ord = 0;
    frame.next_ent = 0;

    // Metadata.
    auto metadata_size = input->read_vint();
    if (!metadata_size) {
        return std::unexpected(std::move(metadata_size.error()));
    }
    if (metadata_size.value() < 0) {
        return err(LuceneError::corrupt_index("negative metadata block size"));
    }
    const size_t num_metadata_bytes = static_cast<size_t>(metadata_size.value());
    if (frame.metadata_bytes.size() < num_metadata_bytes) {
        frame.metadata_bytes.resize(num_metadata_bytes, 0);
    }
    {
        const std::span<uint8_t> target(frame.metadata_bytes);
        if (auto read = input->read_bytes(target.first(num_metadata_bytes)); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    frame.bytes_reader.reset_with_range(std::span<const uint8_t>(frame.metadata_bytes), 0, num_metadata_bytes);

    if (!frame.is_last_in_floor) {
        // Sub-blocks of a single floor block are always written one after
        // another: tail recurse.
        auto fp_end = input->get_file_pointer();
        if (!fp_end) {
            return std::unexpected(std::move(fp_end.error()));
        }
        frame.fp_end = static_cast<size_t>(fp_end.value());
    }
    return {};
}

Result<bool> IntersectTermsEnumFrame::next(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    if (frame.is_leaf_block) {
        if (auto leaf = next_leaf(frame_idx, ite); !leaf) {
            return std::unexpected(std::move(leaf.error()));
        }
        return false;
    }
    return next_non_leaf(frame_idx, ite);
}

VoidResult IntersectTermsEnumFrame::next_leaf(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    frame.next_ent += 1;
    auto suffix = frame.suffix_lengths_reader.read_vint();
    if (!suffix) {
        return std::unexpected(std::move(suffix.error()));
    }
    if (suffix.value() < 0) {
        return err(LuceneError::corrupt_index("negative suffix length"));
    }
    frame.suffix = static_cast<size_t>(suffix.value());
    frame.start_byte_pos = frame.suffixes_reader.get_position();
    if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix)); !skipped) {
        return std::unexpected(std::move(skipped.error()));
    }
    return {};
}

Result<bool> IntersectTermsEnumFrame::next_non_leaf(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    frame.next_ent += 1;
    auto code = frame.suffix_lengths_reader.read_vint();
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    frame.suffix = static_cast<size_t>(static_cast<uint32_t>(code.value()) >> 1);
    frame.start_byte_pos = frame.suffixes_reader.get_position();
    if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix)); !skipped) {
        return std::unexpected(std::move(skipped.error()));
    }
    if ((code.value() & 1) == 0) {
        auto block_state = frame.term_state.get_block_term_state_mut();
        if (!block_state) {
            return std::unexpected(std::move(block_state.error()));
        }
        (*block_state)->term_block_ord += 1;
        return false;
    }
    auto delta = frame.suffix_lengths_reader.read_vlong();
    if (!delta) {
        return std::unexpected(std::move(delta.error()));
    }
    frame.last_sub_fp = static_cast<size_t>(static_cast<int64_t>(frame.fp) - delta.value());
    return true;
}

Result<int32_t> IntersectTermsEnumFrame::get_term_block_ord() const {
    if (is_leaf_block) {
        return next_ent;
    }
    auto block_state = term_state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return (*block_state)->term_block_ord;
}

VoidResult IntersectTermsEnumFrame::decode_meta_data(size_t frame_idx, IntersectTermsEnum &ite) {
    auto &frame = ite.stack()[frame_idx];
    auto limit = frame.get_term_block_ord();
    if (!limit) {
        return std::unexpected(std::move(limit.error()));
    }
    bool absolute = frame.meta_data_upto == 0;

    while (frame.meta_data_upto < limit.value()) {
        auto block_state = frame.term_state.get_block_term_state_mut();
        if (!block_state) {
            return std::unexpected(std::move(block_state.error()));
        }
        auto *state = *block_state;

        if (frame.stats_singleton_run_length > 0) {
            state->doc_freq = 1;
            state->total_term_freq = 1;
            frame.stats_singleton_run_length -= 1;
        } else {
            auto token = frame.stats_reader.read_vint();
            if (!token) {
                return std::unexpected(std::move(token.error()));
            }
            if ((token.value() & 1) == 1) {
                // singleton run
                state->doc_freq = 1;
                state->total_term_freq = 1;
                frame.stats_singleton_run_length = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 1);
            } else {
                state->doc_freq = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 1);
                if (ite.fr().field_info()->index_options == IndexOptions::Docs) {
                    state->total_term_freq = state->doc_freq;
                } else {
                    auto delta = frame.stats_reader.read_vlong();
                    if (!delta) {
                        return std::unexpected(std::move(delta.error()));
                    }
                    state->total_term_freq = static_cast<int64_t>(state->doc_freq) + delta.value();
                }
            }
        }

        BlockTreeTermsParent *parent = ite.fr().parent();
        if (parent == nullptr) {
            return err(LuceneError::number_format("parent not initialized yet"));
        }
        if (auto decoded = parent->postings_reader().decode_term(frame.bytes_reader, *ite.fr().field_info(),
                                                                frame.term_state, absolute);
            !decoded) {
            return std::unexpected(std::move(decoded.error()));
        }

        frame.meta_data_upto += 1;
        absolute = false;
    }

    auto tail = frame.term_state.get_block_term_state_mut();
    if (!tail) {
        return std::unexpected(std::move(tail.error()));
    }
    (*tail)->term_block_ord = frame.meta_data_upto;
    return {};
}

// ===========================================================================
// IntersectTermsEnum
// ===========================================================================

IntersectTermsEnum::IntersectTermsEnum(std::unique_ptr<FieldReader> fr, AutomatonEnum automaton,
                                       std::optional<BytesRef> common_suffix)
    : fr_(std::move(fr)), automaton_(std::move(automaton)), common_suffix_(std::move(common_suffix)) {}

Result<std::unique_ptr<IntersectTermsEnum>> IntersectTermsEnum::create(std::unique_ptr<FieldReader> fr,
                                                                      AutomatonEnum automaton,
                                                                      std::optional<BytesRef> common_suffix,
                                                                      const BytesRef *start_term) {
    if (fr == nullptr) {
        return err(LuceneError::illegal_argument("FieldReader must not be null"));
    }
    BlockTreeTermsParent *parent = fr->parent();
    if (parent == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    auto input = parent->clone_terms_input();
    if (!input) {
        return std::unexpected(std::move(input.error()));
    }

    auto ite = std::unique_ptr<IntersectTermsEnum>(
            new IntersectTermsEnum(std::move(fr), std::move(automaton), std::move(common_suffix)));
    ite->input_ = std::move(input.value());

    auto frame0 = IntersectTermsEnumFrame::create(0, ite->fr());
    if (!frame0) {
        return std::unexpected(std::move(frame0.error()));
    }
    ite->stack_.push_back(std::move(frame0.value()));

    FST *index = ite->fr_->index();
    if (index == nullptr) {
        return err(LuceneError::illegal_state("terms index was not loaded"));
    }

    // The first arc.
    Arc first;
    index->get_first_arc(first);
    const size_t first_arc_idx = 0;
    ite->arcs_.push_back(std::move(first));

    {
        auto &frame = ite->stack_[0];
        frame.fp = static_cast<size_t>(ite->fr_->root_block_fp_);
        frame.fp_orig = frame.fp;
        frame.prefix = 0;
        if (auto state = IntersectTermsEnumFrame::set_state(ite->automaton_, frame, 0); !state) {
            return std::unexpected(std::move(state.error()));
        }
        frame.arc = first_arc_idx;
    }

    if (auto loaded = IntersectTermsEnumFrame::load_from_frame_index_data(ite->fr_->root_code_, first_arc_idx, *ite);
        !loaded) {
        return std::unexpected(std::move(loaded.error()));
    }

    if (start_term != nullptr) {
        ite->saved_start_term_ = *start_term;
    }

    // Push the initial output.
    ite->output_accumulator_.push(arc_output(ite->arcs_[ite->stack_[0].arc].output()));

    if (start_term != nullptr) {
        if (auto seeked = ite->seek_to_start_term(*start_term); !seeked) {
            return std::unexpected(std::move(seeked.error()));
        }
    }

    ite->current_transition_ = ite->current_frame_;
    return ite;
}

VoidResult IntersectTermsEnum::get_frame(size_t ord) {
    if (ord >= stack_.size()) {
        stack_.reserve(ord + 1);
        for (size_t i = stack_.size(); i <= ord; ++i) {
            auto frame = IntersectTermsEnumFrame::create(i, *fr_);
            if (!frame) {
                return std::unexpected(std::move(frame.error()));
            }
            stack_.push_back(std::move(frame.value()));
        }
    }
    return {};
}

Result<size_t> IntersectTermsEnum::get_arc(size_t ord) {
    while (arcs_.size() <= ord) {
        arcs_.push_back(Arc::with_outputs(FstOutput{BytesRef{}}, FstOutput{BytesRef{}}));
    }
    return ord;
}

Result<size_t> IntersectTermsEnum::push_frame(int32_t state) {
    const size_t ord = stack_[current_frame_].ord;
    const size_t new_ord = ord + 1;

    if (auto frame = get_frame(new_ord); !frame) {
        return std::unexpected(std::move(frame.error()));
    }

    const size_t last_sub_fp = stack_[current_frame_].last_sub_fp;
    const size_t prefix = stack_[current_frame_].prefix;
    const size_t suffix = stack_[current_frame_].suffix;

    size_t f_prefix = 0;
    {
        auto &frame = stack_[new_ord];
        frame.fp = last_sub_fp;
        frame.fp_orig = last_sub_fp;
        frame.prefix = prefix + suffix;
        if (auto set = IntersectTermsEnumFrame::set_state(automaton_, frame, state); !set) {
            return std::unexpected(std::move(set.error()));
        }
        f_prefix = frame.prefix;
    }

    // Walk the arc through the index: this is only done to get the floor data
    // from the index and skip floor blocks when possible.
    size_t arc_idx = stack_[current_frame_].arc;
    size_t idx = prefix;
    const size_t init_output_count = output_accumulator_.output_count();

    FST *index = fr_->index();
    if (index == nullptr) {
        return err(LuceneError::illegal_state("terms index was not loaded"));
    }

    while (idx < f_prefix) {
        const int32_t target = static_cast<int32_t>(term_.bytes_mut().bytes()[idx]);
        auto next_idx = get_arc(idx + 1);
        if (!next_idx) {
            return std::unexpected(std::move(next_idx.error()));
        }
        auto found = index->find_target_arc(target, &arcs_[arc_idx], arcs_[next_idx.value()], index->reader());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        arc_idx = next_idx.value();
        output_accumulator_.push(arc_output(arcs_[arc_idx].output()));
        idx += 1;
    }

    {
        auto &frame = stack_[new_ord];
        frame.arc = arc_idx;
        frame.output_num = output_accumulator_.output_count() - init_output_count;
    }

    output_accumulator_.push(arc_output(arcs_[stack_[new_ord].arc].next_final_output()));

    if (auto loaded = IntersectTermsEnumFrame::load_from_output_accumulator(new_ord, *this); !loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    output_accumulator_.pop(arc_output(arcs_[stack_[new_ord].arc].next_final_output()));
    return new_ord;
}

Result<int32_t> IntersectTermsEnum::get_state() {
    const auto &frame = stack_[current_frame_];
    int32_t state = frame.state;
    const auto suffix_bytes = frame.suffixes_reader.bytes();
    for (size_t idx = 0; idx < frame.suffix; ++idx) {
        auto next = automaton_.step(state, static_cast<int32_t>(suffix_bytes[frame.start_byte_pos + idx]));
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        state = next.value();
    }
    return state;
}

VoidResult IntersectTermsEnum::seek_to_start_term(const BytesRef &target) {
    if (term_.length() < target.length()) {
        if (auto grown = term_.grow(target.length()); !grown) {
            return std::unexpected(std::move(grown.error()));
        }
    }
    term_.set_length(target.length());

    const auto target_bytes = target.as_slice();

    // NOTE: this is a seekFloor: the first `next()` returns the term *after*
    // the requested one, matching the reference implementation.
    for (size_t i = 0; i <= target.length(); ++i) {
        while (true) {
            const size_t frame_idx = current_frame_;
            const int32_t sav_next_ent = stack_[frame_idx].next_ent;
            const size_t save_pos = stack_[frame_idx].suffixes_reader.get_position();
            const size_t save_len_pos = stack_[frame_idx].suffix_lengths_reader.get_position();
            const size_t save_start_byte_pos = stack_[frame_idx].start_byte_pos;
            const size_t save_suffix = stack_[frame_idx].suffix;
            const size_t save_last_sub_fp = stack_[frame_idx].last_sub_fp;
            auto saved_block_ord = stack_[frame_idx].term_state.get_block_term_state();
            if (!saved_block_ord) {
                return std::unexpected(std::move(saved_block_ord.error()));
            }
            const int32_t save_term_block_ord = (*saved_block_ord)->term_block_ord;

            auto is_sub_block_result = IntersectTermsEnumFrame::next(frame_idx, *this);
            if (!is_sub_block_result) {
                return std::unexpected(std::move(is_sub_block_result.error()));
            }
            const bool is_sub_block = is_sub_block_result.value();

            {
                auto &frame = stack_[frame_idx];
                term_.set_length(frame.prefix + frame.suffix);
                if (term_.bytes_mut().bytes().size() < term_.length()) {
                    if (auto grown = term_.grow(term_.length()); !grown) {
                        return std::unexpected(std::move(grown.error()));
                    }
                }
                const auto source = frame.suffixes_reader.bytes();
                std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(frame.start_byte_pos),
                            static_cast<std::ptrdiff_t>(frame.suffix),
                            term_.bytes_mut().bytes().begin() + static_cast<std::ptrdiff_t>(frame.prefix));
            }

            const bool target_starts_with_term =
                    term_.length() <= target.length() &&
                    std::ranges::equal(term_.get_bytes_ref().as_slice(), target_bytes.first(term_.length()));

            if (is_sub_block && target_starts_with_term) {
                // Recurse.
                auto state = get_state();
                if (!state) {
                    return std::unexpected(std::move(state.error()));
                }
                auto pushed = push_frame(state.value());
                if (!pushed) {
                    return std::unexpected(std::move(pushed.error()));
                }
                current_frame_ = pushed.value();
                break;
            }

            const int32_t cmp = compare_bytes(term_.get_bytes_ref().as_slice(), target_bytes);
            if (cmp < 0) {
                if (stack_[frame_idx].next_ent == stack_[frame_idx].ent_count) {
                    if (!stack_[frame_idx].is_last_in_floor) {
                        if (auto loaded = IntersectTermsEnumFrame::load_next_floor_block(frame_idx, *this); !loaded) {
                            return std::unexpected(std::move(loaded.error()));
                        }
                        continue;
                    }
                    return {};
                }
                continue;
            }
            if (cmp == 0) {
                return {};
            }

            // Fallback to the prior entry: the semantics of this method is that
            // the first call to `next()` returns the term after the requested
            // term.
            {
                auto &frame = stack_[frame_idx];
                frame.next_ent = sav_next_ent;
                frame.last_sub_fp = save_last_sub_fp;
                frame.start_byte_pos = save_start_byte_pos;
                frame.suffix = save_suffix;
                frame.suffixes_reader.set_position(save_pos);
                frame.suffix_lengths_reader.set_position(save_len_pos);
                auto block_state = frame.term_state.get_block_term_state_mut();
                if (!block_state) {
                    return std::unexpected(std::move(block_state.error()));
                }
                (*block_state)->term_block_ord = save_term_block_ord;
            }
            {
                auto &frame = stack_[frame_idx];
                const auto source = frame.suffixes_reader.bytes();
                std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(frame.start_byte_pos),
                            static_cast<std::ptrdiff_t>(frame.suffix),
                            term_.bytes_mut().bytes().begin() + static_cast<std::ptrdiff_t>(frame.prefix));
                term_.set_length(frame.prefix + frame.suffix);
            }
            return {};
        }
    }
    return {};
}

Result<bool> IntersectTermsEnum::pop_push_next() {
    // Pop finished frames.
    while (true) {
        const size_t frame_idx = current_frame_;
        if (stack_[frame_idx].next_ent != stack_[frame_idx].ent_count) {
            break;
        }
        if (!stack_[frame_idx].is_last_in_floor) {
            // Advance to the next floor block.
            if (auto loaded = IntersectTermsEnumFrame::load_next_floor_block(frame_idx, *this); !loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            break;
        }
        const size_t ord = stack_[frame_idx].ord;
        if (ord == 0) {
            return err(LuceneError::no_more_terms(""));
        }
        output_accumulator_.pop_n(stack_[frame_idx].output_num);
        current_frame_ = ord - 1;
        current_transition_ = current_frame_;
    }
    return IntersectTermsEnumFrame::next(current_frame_, *this);
}

Result<const BytesRef *> IntersectTermsEnum::next_() {
    bool is_sub_block = false;
    {
        auto stepped = pop_push_next();
        if (!stepped) {
            return std::unexpected(std::move(stepped.error()));
        }
        is_sub_block = stepped.value();
    }

    while (true) {
    next_term:
        int32_t state = 0;
        int32_t last_state = 0;

        const size_t suffix = stack_[current_frame_].suffix;
        // NOTE: suffix == 0 can only happen on the first term in a block, when
        // there is a term exactly matching a prefix in the index.
        if (suffix != 0) {
            const auto &frame = stack_[current_frame_];
            const auto suffix_bytes = frame.suffixes_reader.bytes();
            const int32_t label = static_cast<int32_t>(suffix_bytes[frame.start_byte_pos]);

            if (label < stack_[current_transition_].transition.min) {
                // Common case: we are scanning terms in this block to catch up
                // to the current transition in the automaton.
                const int32_t min_transition = stack_[current_transition_].transition.min;
                while (stack_[current_frame_].next_ent < stack_[current_frame_].ent_count) {
                    auto stepped = IntersectTermsEnumFrame::next(current_frame_, *this);
                    if (!stepped) {
                        return std::unexpected(std::move(stepped.error()));
                    }
                    is_sub_block = stepped.value();
                    const auto &frame_now = stack_[current_frame_];
                    const auto bytes = frame_now.suffixes_reader.bytes();
                    if (static_cast<int32_t>(bytes[frame_now.start_byte_pos]) >= min_transition) {
                        goto next_term;
                    }
                }
                // End of frame.
                auto stepped = pop_push_next();
                if (!stepped) {
                    return std::unexpected(std::move(stepped.error()));
                }
                is_sub_block = stepped.value();
                goto next_term;
            }

            int32_t transition_max = stack_[current_transition_].transition.max;
            // Advance where we are in the automaton to match this label.
            while (label > transition_max) {
                auto &frame = stack_[current_frame_];
                if (frame.transition_index >= frame.transition_count - 1) {
                    // Pop this frame: no further matches are possible because we
                    // have moved beyond what the max transition allows.
                    if (frame.ord == 0) {
                        return static_cast<const BytesRef *>(nullptr);
                    }
                    const size_t output_num = frame.output_num;
                    output_accumulator_.pop_n(output_num);
                    const size_t parent_ord = frame.ord - 1;
                    current_frame_ = parent_ord;
                    current_transition_ = current_frame_;
                    auto stepped = pop_push_next();
                    if (!stepped) {
                        return std::unexpected(std::move(stepped.error()));
                    }
                    is_sub_block = stepped.value();
                    goto next_term;
                }

                frame.transition_index += 1;
                if (auto next = automaton_.get_next_transition(stack_[current_transition_].transition); !next) {
                    return std::unexpected(std::move(next.error()));
                }

                if (label < stack_[current_transition_].transition.min) {
                    const int32_t min_transition = stack_[current_transition_].transition.min;
                    bool more = stack_[current_frame_].next_ent < stack_[current_frame_].ent_count;
                    while (more) {
                        auto stepped = IntersectTermsEnumFrame::next(current_frame_, *this);
                        if (!stepped) {
                            return std::unexpected(std::move(stepped.error()));
                        }
                        is_sub_block = stepped.value();
                        const auto &frame_now = stack_[current_frame_];
                        const auto bytes = frame_now.suffixes_reader.bytes();
                        if (static_cast<int32_t>(bytes[frame_now.start_byte_pos]) >= min_transition) {
                            goto next_term;
                        }
                        more = frame_now.next_ent < frame_now.ent_count;
                    }
                    // End of frame.
                    auto stepped = pop_push_next();
                    if (!stepped) {
                        return std::unexpected(std::move(stepped.error()));
                    }
                    is_sub_block = stepped.value();
                    goto next_term;
                }
                transition_max = stack_[current_transition_].transition.max;
            }

            if (common_suffix_.has_value()) {
                const auto &suffix_ref = common_suffix_.value();
                const auto &frame = stack_[current_frame_];
                if (!is_sub_block) {
                    const size_t term_len = frame.prefix + frame.suffix;
                    if (term_len < suffix_ref.length()) {
                        auto stepped = pop_push_next();
                        if (!stepped) {
                            return std::unexpected(std::move(stepped.error()));
                        }
                        is_sub_block = stepped.value();
                        goto next_term;
                    }

                    size_t suffix_bytes_pos = 0;
                    size_t common_suffix_bytes_pos = 0;
                    const auto suffix_ref_bytes = suffix_ref.as_slice();
                    if (suffix_ref.length() > frame.suffix) {
                        // A prefix of the common suffix overlaps with the suffix
                        // of the block prefix, so first test whether the prefix
                        // part matches.
                        const size_t len_in_prefix = suffix_ref.length() - frame.suffix;
                        size_t term_bytes_pos = frame.prefix - len_in_prefix;
                        const size_t term_bytes_end = frame.prefix;
                        bool mismatch = false;
                        while (term_bytes_pos < term_bytes_end) {
                            if (term_.bytes_mut().bytes()[term_bytes_pos] != suffix_ref_bytes[common_suffix_bytes_pos]) {
                                mismatch = true;
                                break;
                            }
                            term_bytes_pos += 1;
                            common_suffix_bytes_pos += 1;
                        }
                        if (mismatch) {
                            auto stepped = pop_push_next();
                            if (!stepped) {
                                return std::unexpected(std::move(stepped.error()));
                            }
                            is_sub_block = stepped.value();
                            goto next_term;
                        }
                        suffix_bytes_pos = frame.start_byte_pos;
                    } else {
                        suffix_bytes_pos = frame.start_byte_pos + frame.suffix - suffix_ref.length();
                    }

                    const size_t common_suffix_bytes_pos_end = suffix_ref.length();
                    while (common_suffix_bytes_pos < common_suffix_bytes_pos_end) {
                        const auto current_suffix_bytes = stack_[current_frame_].suffixes_reader.bytes();
                        if (current_suffix_bytes[suffix_bytes_pos] != suffix_ref_bytes[common_suffix_bytes_pos]) {
                            auto stepped = pop_push_next();
                            if (!stepped) {
                                return std::unexpected(std::move(stepped.error()));
                            }
                            is_sub_block = stepped.value();
                            goto next_term;
                        }
                        suffix_bytes_pos += 1;
                        common_suffix_bytes_pos += 1;
                    }
                }
            }

            {
                const auto &frame = stack_[current_frame_];
                last_state = frame.state;
                state = stack_[current_transition_].transition.dest;

                const auto bytes = frame.suffixes_reader.bytes();
                const size_t end = frame.start_byte_pos + frame.suffix;
                size_t idx = frame.start_byte_pos + 1;
                while (idx < end) {
                    last_state = state;
                    auto next = automaton_.step(state, static_cast<int32_t>(bytes[idx]));
                    if (!next) {
                        return std::unexpected(std::move(next.error()));
                    }
                    state = next.value();
                    if (state == -1) {
                        auto stepped = pop_push_next();
                        if (!stepped) {
                            return std::unexpected(std::move(stepped.error()));
                        }
                        is_sub_block = stepped.value();
                        goto next_term;
                    }
                    idx += 1;
                }
            }
        } else {
            state = stack_[current_frame_].state;
            last_state = stack_[current_frame_].last_state;
        }

        if (is_sub_block) {
            if (auto copied = copy_term(); !copied) {
                return std::unexpected(std::move(copied.error()));
            }
            auto pushed = push_frame(state);
            if (!pushed) {
                return std::unexpected(std::move(pushed.error()));
            }
            current_frame_ = pushed.value();
            current_transition_ = current_frame_;
            stack_[pushed.value()].last_state = last_state;
        } else {
            auto accepted = automaton_.is_accept(state);
            if (!accepted) {
                return std::unexpected(std::move(accepted.error()));
            }
            if (accepted.value()) {
                if (auto copied = copy_term(); !copied) {
                    return std::unexpected(std::move(copied.error()));
                }
                return &term_.get_bytes_ref();
            }
        }

        auto stepped = pop_push_next();
        if (!stepped) {
            return std::unexpected(std::move(stepped.error()));
        }
        is_sub_block = stepped.value();
    }
}

VoidResult IntersectTermsEnum::copy_term() {
    const auto &frame = stack_[current_frame_];
    const size_t len = frame.prefix + frame.suffix;

    if (term_.length() < len) {
        if (auto grown = term_.grow(len); !grown) {
            return std::unexpected(std::move(grown.error()));
        }
    }

    const auto source = frame.suffixes_reader.bytes();
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(frame.start_byte_pos),
                static_cast<std::ptrdiff_t>(frame.suffix),
                term_.bytes_mut().bytes().begin() + static_cast<std::ptrdiff_t>(frame.prefix));
    term_.set_length(len);
    return {};
}

Result<const BytesRef *> IntersectTermsEnum::next() {
    auto value = next_();
    if (value) {
        return value;
    }
    if (value.error().kind() == ErrorKind::kNoMoreTerms) {
        return static_cast<const BytesRef *>(nullptr);
    }
    return std::unexpected(std::move(value.error()));
}

Result<AttributeSource *> IntersectTermsEnum::attributes() {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum has no attributes"));
}

Result<AttributeSource *> IntersectTermsEnum::attributes_mut() {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum has no attributes"));
}

Result<bool> IntersectTermsEnum::seek_exact(std::span<const uint8_t> /*term*/) {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum::seek_exact"));
}

Result<bool> IntersectTermsEnum::prepare_seek_exact(std::span<const uint8_t> /*term*/) {
    return true;
}

Result<bool> IntersectTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<SeekStatus> IntersectTermsEnum::seek_ceil(std::span<const uint8_t> /*term*/) {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum::seek_ceil"));
}

VoidResult IntersectTermsEnum::seek_exact_with_ord(int64_t /*ord*/) {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum::seek_exact_with_ord"));
}

VoidResult IntersectTermsEnum::seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum & /*state*/) {
    auto found = seek_exact(term);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    if (!found.value()) {
        return err(LuceneError::illegal_argument("term does not exist"));
    }
    return {};
}

Result<const BytesRef *> IntersectTermsEnum::term() const {
    return &term_.get_bytes_ref();
}

Result<int64_t> IntersectTermsEnum::ord() const {
    return err(LuceneError::unsupported_operation("IntersectTermsEnum::ord"));
}

Result<int32_t> IntersectTermsEnum::doc_freq() {
    if (auto decoded = IntersectTermsEnumFrame::decode_meta_data(current_frame_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto block_state = stack_[current_frame_].term_state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return (*block_state)->doc_freq;
}

Result<int64_t> IntersectTermsEnum::total_term_freq() {
    if (auto decoded = IntersectTermsEnumFrame::decode_meta_data(current_frame_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto block_state = stack_[current_frame_].term_state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return (*block_state)->total_term_freq;
}

Result<std::unique_ptr<PostingsEnum>> IntersectTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                             int32_t flags) {
    if (auto decoded = IntersectTermsEnumFrame::decode_meta_data(current_frame_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    BlockTreeTermsParent *parent = fr_->parent();
    if (parent == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    auto postings = parent->postings_reader().postings(*fr_->field_info(), stack_[current_frame_].term_state,
                                                       std::move(reuse), flags);
    if (!postings) {
        return std::unexpected(std::move(postings.error()));
    }
    if (postings.value() == nullptr) {
        return err(LuceneError::illegal_state("could not get postings enum"));
    }
    return postings;
}

Result<std::unique_ptr<ImpactsEnum>> IntersectTermsEnum::impacts(int32_t flags) {
    if (auto decoded = IntersectTermsEnumFrame::decode_meta_data(current_frame_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    BlockTreeTermsParent *parent = fr_->parent();
    if (parent == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    return parent->postings_reader().impacts(*fr_->field_info(), stack_[current_frame_].term_state, flags);
}

Result<TermStateEnum> IntersectTermsEnum::term_state() {
    if (auto decoded = IntersectTermsEnumFrame::decode_meta_data(current_frame_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return stack_[current_frame_].term_state;
}

// ===========================================================================
// FieldReader::intersect
// ===========================================================================

Result<std::unique_ptr<TermsEnum>> FieldReader::intersect(const CompiledAutomaton &compiled,
                                                          const BytesRef *start_term) const {
    if (compiled.type() != AutomatonType::kNormal) {
        return err(LuceneError::illegal_argument("please use CompiledAutomaton.get_terms_enum instead"));
    }
    auto automaton = compiled.get_automaton();
    if (!automaton) {
        return std::unexpected(std::move(automaton.error()));
    }
    auto copy = clone_for_enum();
    if (!copy) {
        return std::unexpected(std::move(copy.error()));
    }
    auto ite = IntersectTermsEnum::create(std::move(copy.value()), std::move(automaton.value()),
                                          compiled.common_suffix_ref(), start_term);
    if (!ite) {
        return std::unexpected(std::move(ite.error()));
    }
    return std::unique_ptr<TermsEnum>(std::move(ite.value()));
}

} // namespace lucene
