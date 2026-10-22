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

module lucene_core:block_tree_segment_terms_enum.impl;

import :block_tree_segment_terms_enum;

import :block_term_state;
import :block_tree_msb_vlong;
import :block_tree_output_accumulator;
import :block_tree_terms_constants;
import :bytes_ref;
import :bytes_ref_builder;
import :compression_algorithm;
import :data_input;
import :debug;
import :error;
import :fst;
import :field_infos;
import :fst_byte_sequence_outputs;
import :fst_store;
import :index_input;
import :postings_enum;
import :impacts_enum;
import :postings_reader_base;
import :terms;
import :terms;
import :terms_enum;
import :types;

import std;

namespace lucene {

namespace {

/// The reference implementation's `Arc<BytesRef>` output slots are type erased (`std::any`) in this
/// port; `ByteSequenceOutputs` always stores a [`BytesRef`], and a missing slot
/// means `NO_OUTPUT`.
const BytesRef &arc_output(const std::any &slot) {
    const auto *output = std::any_cast<BytesRef>(&slot);
    return output != nullptr ? *output : no_output();
}

std::any no_output_slot() { return std::any{BytesRef{}}; }

int32_t compare_spans(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    const size_t shared = std::min(left.size(), right.size());
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

} // namespace

Result<std::unique_ptr<SegmentTermsEnum>> SegmentTermsEnum::create(std::unique_ptr<FieldReader> fr) {
    if (fr == nullptr) {
        return err(LuceneError::illegal_argument("FieldReader must not be null"));
    }
    auto ste = std::unique_ptr<SegmentTermsEnum>(new SegmentTermsEnum());
    ste->fr_ = std::move(fr);

    // The static frame holds the state of a seek by term state, or a cached
    // seek.
    auto static_frame = SegmentTermsEnumFrame::create(-1, *ste->fr_);
    if (!static_frame) {
        return std::unexpected(std::move(static_frame.error()));
    }
    ste->stack_.push_back(std::move(static_frame.value()));

    if (FST *index = ste->fr_->index(); index != nullptr) {
        Arc first;
        index->get_first_arc(first);
        debug_check(first.is_final());
        ste->arcs_.push_back(std::move(first));
    } else {
        ste->arcs_.emplace_back();
    }
    return ste;
}

VoidResult SegmentTermsEnum::init_index_input() {
    if (input_ != nullptr) {
        return {};
    }
    if (fr_->parent() == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    auto cloned = fr_->parent()->clone_terms_input();
    if (!cloned) {
        return std::unexpected(std::move(cloned.error()));
    }
    input_ = std::move(cloned.value());
    return {};
}

Result<size_t> SegmentTermsEnum::get_frame(size_t ord) {
    const size_t frame_idx = ord + 1;
    while (stack_.size() <= frame_idx) {
        const auto frame_ord = static_cast<int32_t>(stack_.size() - 1);
        auto frame = SegmentTermsEnumFrame::create(frame_ord, *fr_);
        if (!frame) {
            return std::unexpected(std::move(frame.error()));
        }
        stack_.push_back(std::move(frame.value()));
    }
    debug_check(stack_[frame_idx].ord == static_cast<int32_t>(ord));
    return frame_idx;
}

Result<size_t> SegmentTermsEnum::get_arc(size_t ord) {
    while (arcs_.size() <= ord) {
        arcs_.push_back(Arc::with_outputs(no_output_slot(), no_output_slot()));
    }
    return ord;
}

Result<size_t> SegmentTermsEnum::push_frame(std::optional<size_t> arc, size_t fp, size_t length) {
    const size_t frame_idx = stack_.size() > current_frame_idx_ ? current_frame_idx_ + 1 : 0;
    const int32_t ord = stack_[current_frame_idx_].ord + 1;
    auto grown = get_frame(static_cast<size_t>(ord));
    if (!grown) {
        return std::unexpected(std::move(grown.error()));
    }
    (void)frame_idx;
    auto &frame = stack_[grown.value()];
    frame.arc = arc;

    if (frame.fp_orig == fp && frame.next_ent != -1) {
        if (static_cast<int32_t>(frame.ord) > target_before_current_length_) {
            if (auto rewound = frame.rewind(); !rewound) {
                return std::unexpected(std::move(rewound.error()));
            }
        }
        debug_check(length == frame.prefix_length);
    } else {
        frame.next_ent = -1;
        frame.prefix_length = length;
        auto block_state = frame.state.get_block_term_state_mut();
        if (!block_state) {
            return std::unexpected(std::move(block_state.error()));
        }
        (*block_state)->term_block_ord = 0;
        frame.fp_orig = fp;
        frame.fp = fp;
        frame.last_sub_fp.reset();
    }
    return grown.value();
}

Result<size_t> SegmentTermsEnum::push_frame_with_data(std::optional<size_t> arc, const BytesRef &term_bytes,
                                                      size_t length) {
    output_accumulator_.reset();
    output_accumulator_.push(term_bytes);
    return push_frame_with_length(arc, length);
}

Result<size_t> SegmentTermsEnum::push_frame_with_length(std::optional<size_t> arc, size_t length) {
    output_accumulator_.prepare_read();
    auto code = fr_->read_vlong_output(output_accumulator_);
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    const size_t fp_seek = static_cast<size_t>(static_cast<uint64_t>(code.value()) >> OUTPUT_FLAGS_NUM_BITS);

    const int32_t current_ord = stack_[current_frame_idx_].ord;
    const size_t ord = static_cast<size_t>(current_ord + 1);
    auto frame_idx = get_frame(ord);
    if (!frame_idx) {
        return std::unexpected(std::move(frame_idx.error()));
    }
    auto &frame = stack_[frame_idx.value()];
    frame.has_terms = (code.value() & OUTPUT_FLAG_HAS_TERMS) != 0;
    frame.has_terms_orig = frame.has_terms;
    frame.is_floor = (code.value() & OUTPUT_FLAG_IS_FLOOR) != 0;

    if (frame.is_floor) {
        if (auto floor = frame.set_floor_data(output_accumulator_); !floor) {
            return std::unexpected(std::move(floor.error()));
        }
    }
    auto pushed = push_frame(arc, fp_seek, length);
    if (!pushed) {
        return std::unexpected(std::move(pushed.error()));
    }
    debug_check(pushed.value() == frame_idx.value());
    return frame_idx.value();
}

Result<const BytesRef *> SegmentTermsEnum::next() {
    if (input_ == nullptr) {
        std::optional<size_t> arc;
        if (FST *index = fr_->index(); index != nullptr) {
            index->get_first_arc(arcs_[0]);
            debug_check(arcs_[0].is_final());
            arc = 0;
        }
        auto pushed = push_frame_with_data(arc, fr_->root_code_, 0);
        if (!pushed) {
            return std::unexpected(std::move(pushed.error()));
        }
        current_frame_idx_ = pushed.value();
        auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
    }
    target_before_current_length_ = stack_[current_frame_idx_].ord;
    debug_check(!eof_);

    if (current_frame_idx_ == STATIC_FRAME_IDX) {
        // A seek may have left the static frame positioned on a term; catch the
        // internal state up so that `next` works from here.
        BytesRefBuilder saved = term_;
        auto found = seek_exact(term_.get_bytes_ref().as_slice());
        term_ = std::move(saved);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        debug_check(found.value());
    }

    while (true) {
        if (stack_[current_frame_idx_].next_ent == stack_[current_frame_idx_].ent_count) {
            if (!stack_[current_frame_idx_].is_last_in_floor) {
                // Advance the floor block.
                auto loaded = SegmentTermsEnumFrame::load_next_floor_block(current_frame_idx_, *this);
                if (!loaded) {
                    return std::unexpected(std::move(loaded.error()));
                }
                break;
            }

            const int32_t current_ord = stack_[current_frame_idx_].ord;
            const size_t last_fp = stack_[current_frame_idx_].fp_orig;
            if (current_ord == 0) {
                eof_ = true;
                term_.clear();
                valid_index_prefix_ = 0;
                if (auto rewound = stack_[current_frame_idx_].rewind(); !rewound) {
                    return std::unexpected(std::move(rewound.error()));
                }
                term_exists_ = false;
                return static_cast<const BytesRef *>(nullptr);
            }

            current_frame_idx_ = static_cast<size_t>(current_ord);
            const int32_t next_ent = stack_[current_frame_idx_].next_ent;
            const std::optional<size_t> last_sub_fp = stack_[current_frame_idx_].last_sub_fp;

            if (next_ent == -1 || !last_sub_fp.has_value() || last_sub_fp.value() != last_fp) {
                // Popped into a frame that is not loaded yet or not scanned to
                // the right entry.
                if (auto scanned = SegmentTermsEnumFrame::scan_to_floor_frame(current_frame_idx_, *this);
                    !scanned) {
                    return std::unexpected(std::move(scanned.error()));
                }
                auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
                if (!loaded) {
                    return std::unexpected(std::move(loaded.error()));
                }
                if (auto positioned = SegmentTermsEnumFrame::scan_to_sub_block(current_frame_idx_, last_fp, *this);
                    !positioned) {
                    return std::unexpected(std::move(positioned.error()));
                }
            }
            valid_index_prefix_ = std::min(valid_index_prefix_, stack_[current_frame_idx_].prefix_length);
        } else {
            break;
        }
    }

    while (true) {
        auto has_next = SegmentTermsEnumFrame::next(current_frame_idx_, *this);
        if (!has_next) {
            return std::unexpected(std::move(has_next.error()));
        }
        if (has_next.value()) {
            const std::optional<size_t> last_sub_fp = stack_[current_frame_idx_].last_sub_fp;
            if (!last_sub_fp.has_value()) {
                return err(LuceneError::illegal_state("term block has no current sub-block"));
            }
            const size_t length = term_.length();
            auto pushed = push_frame(std::nullopt, last_sub_fp.value(), length);
            if (!pushed) {
                return std::unexpected(std::move(pushed.error()));
            }
            current_frame_idx_ = pushed.value();
            // A "next" frame is never treated as a floor frame, so that we do
            // not scan to the right floor frame.
            auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
            if (!loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            continue;
        }
        return &term_.get_bytes_ref();
    }
}

Result<bool> SegmentTermsEnum::seek_exact(std::span<const uint8_t> target) {
    auto prepared = prepare_seek_exact_with_prefetch(target, false);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    if (!prepared.value()) {
        return false;
    }
    return get_prepare_seek_exact_status(target);
}

Result<bool> SegmentTermsEnum::prepare_seek_exact(std::span<const uint8_t> target) {
    return prepare_seek_exact_with_prefetch(target, true);
}

Result<bool> SegmentTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    switch (prepare_status_) {
        case PrepareStatus::NotFound:
            return false;
        case PrepareStatus::Found:
            return true;
        case PrepareStatus::Pending: {
            auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
            if (!loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            auto scanned = SegmentTermsEnumFrame::scan_to_term(current_frame_idx_, target, true, *this);
            if (!scanned) {
                return std::unexpected(std::move(scanned.error()));
            }
            return scanned.value() == SeekStatus::Found;
        }
    }
    return err(LuceneError::illegal_state("unknown prepare status"));
}

Result<bool> SegmentTermsEnum::prepare_seek_exact_with_prefetch(std::span<const uint8_t> target, bool prefetch) {
    FST *index = fr_->index();
    if (index == nullptr) {
        return err(LuceneError::illegal_state("terms index was not loaded"));
    }
    if (fr_->num_terms_ > 0) {
        const int32_t min_cmp = compare_spans(target, fr_->min_term_.as_slice());
        const int32_t max_cmp = compare_spans(target, fr_->max_term_.as_slice());
        if (min_cmp < 0 || max_cmp > 0) {
            prepare_status_ = PrepareStatus::NotFound;
            return false;
        }
    }

    if (auto grown = term_.grow(1 + target.size()); !grown) {
        return std::unexpected(std::move(grown.error()));
    }
    eof_ = false;

    size_t target_upto = 0;
    const int32_t target_before_current_length = stack_[current_frame_idx_].ord;
    target_before_current_length_ = target_before_current_length;
    output_accumulator_.reset();

    size_t arc_index = 0;
    if (current_frame_idx_ != STATIC_FRAME_IDX) {
        // Already positioned somewhere: reuse the common prefix with the
        // current term.
        arc_index = 0;
        debug_check(arcs_[arc_index].is_final());
        output_accumulator_.push(arc_output(arcs_[arc_index].output()));

        target_upto = 0;
        size_t last_frame = 1;
        debug_check(valid_index_prefix_ <= term_.length());

        const size_t target_limit = std::min(target.size(), valid_index_prefix_);
        int32_t cmp = 0;

        while (target_upto < target_limit) {
            const int32_t term_byte = static_cast<int32_t>(term_.byte_at(target_upto));
            const int32_t target_byte = static_cast<int32_t>(target[target_upto]);
            cmp = term_byte - target_byte;
            if (cmp != 0) {
                break;
            }
            arc_index = 1 + target_upto;
            debug_check(arcs_[arc_index].label() == target_byte);
            output_accumulator_.push(arc_output(arcs_[arc_index].output()));

            if (arcs_[arc_index].is_final()) {
                last_frame = static_cast<size_t>(stack_[last_frame].ord + 2);
            }
            target_upto += 1;
        }

        if (cmp == 0) {
            // Compare the rest of the term to learn whether the target is
            // before, equal to or after the current term.
            const std::span<const uint8_t> current = term_.get_bytes_ref().as_slice();
            cmp = compare_spans(current.subspan(target_upto), target.subspan(target_upto));
        }

        if (cmp < 0) {
            current_frame_idx_ = last_frame;
        } else if (cmp > 0) {
            // The target is before the current term: keep the frame but rewind
            // it so it rescans from the start.
            target_before_current_length_ = stack_[last_frame].ord;
            current_frame_idx_ = last_frame;
            if (auto rewound = stack_[last_frame].rewind(); !rewound) {
                return std::unexpected(std::move(rewound.error()));
            }
        } else {
            debug_check(term_.length() == target.size());
            if (term_exists_) {
                prepare_status_ = PrepareStatus::Found;
                return true;
            }
        }
    } else {
        arc_index = 0;
        target_before_current_length_ = -1;
        index->get_first_arc(arcs_[0]);
        debug_check(arcs_[0].is_final());
        output_accumulator_.push(arc_output(arcs_[0].output()));
        current_frame_idx_ = STATIC_FRAME_IDX;

        target_upto = 0;
        const BytesRef next_final_output = arc_output(arcs_[0].next_final_output());
        output_accumulator_.push(next_final_output);
        auto pushed = push_frame_with_length(0, 0);
        if (!pushed) {
            return std::unexpected(std::move(pushed.error()));
        }
        current_frame_idx_ = pushed.value();
        output_accumulator_.pop(next_final_output);
    }

    // Continue walking the index.
    while (target_upto < target.size()) {
        const int32_t target_label = static_cast<int32_t>(target[target_upto]);

        auto next_arc_idx_result = get_arc(1 + target_upto);
        if (!next_arc_idx_result) {
            return std::unexpected(std::move(next_arc_idx_result.error()));
        }
        const size_t next_arc_idx = next_arc_idx_result.value();

        debug_check(arc_index < next_arc_idx);
        auto found = index->find_target_arc(target_label, &arcs_[arc_index], arcs_[next_arc_idx], index->reader());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }

        if (!found.value()) {
            // The index is exhausted: the deepest loaded frame is the block that
            // must hold the term, so scan that block instead of giving up.
            valid_index_prefix_ = stack_[current_frame_idx_].prefix_length;
            if (auto scanned =
                        SegmentTermsEnumFrame::scan_to_floor_frame_with_target(current_frame_idx_, target, *this, true);
                !scanned) {
                return std::unexpected(std::move(scanned.error()));
            }
            if (!stack_[current_frame_idx_].has_terms) {
                term_exists_ = false;
                term_.set_byte_at(target_upto, static_cast<uint8_t>(target_label));
                term_.set_length(target_upto + 1);
                prepare_status_ = PrepareStatus::NotFound;
                return false;
            }
            if (prefetch) {
                if (auto prefetched = SegmentTermsEnumFrame::prefetch_block(current_frame_idx_, *this); !prefetched) {
                    return std::unexpected(std::move(prefetched.error()));
                }
            }
            prepare_status_ = PrepareStatus::Pending;
            return true;
        }

        arc_index = next_arc_idx;
        term_.set_byte_at(target_upto, static_cast<uint8_t>(target_label));
        output_accumulator_.push(arc_output(arcs_[arc_index].output()));
        target_upto += 1;
        if (arcs_[arc_index].is_final()) {
            const BytesRef next_final_output = arc_output(arcs_[arc_index].next_final_output());
            output_accumulator_.push(next_final_output);
            auto pushed = push_frame_with_length(arc_index, target_upto);
            if (!pushed) {
                return std::unexpected(std::move(pushed.error()));
            }
            current_frame_idx_ = pushed.value();
            output_accumulator_.pop(next_final_output);
        }
    }

    valid_index_prefix_ = stack_[current_frame_idx_].prefix_length;

    if (auto scanned =
                SegmentTermsEnumFrame::scan_to_floor_frame_with_target(current_frame_idx_, target, *this, true);
        !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    if (!stack_[current_frame_idx_].has_terms) {
        term_exists_ = false;
        term_.set_length(target_upto);
        prepare_status_ = PrepareStatus::NotFound;
        return false;
    }

    if (prefetch) {
        if (auto prefetched = SegmentTermsEnumFrame::prefetch_block(current_frame_idx_, *this); !prefetched) {
            return std::unexpected(std::move(prefetched.error()));
        }
    }
    prepare_status_ = PrepareStatus::Pending;
    return true;
}

Result<SeekStatus> SegmentTermsEnum::seek_ceil(std::span<const uint8_t> target) {
    FST *index = fr_->index();
    if (index == nullptr) {
        return err(LuceneError::illegal_state("terms index was not loaded"));
    }
    if (auto grown = term_.grow(1 + target.size()); !grown) {
        return std::unexpected(std::move(grown.error()));
    }
    eof_ = false;

    size_t target_upto = 0;
    target_before_current_length_ = stack_[current_frame_idx_].ord;
    output_accumulator_.reset();

    size_t arc_index = 0;
    if (current_frame_idx_ != STATIC_FRAME_IDX) {
        arc_index = 0;
        debug_check(arcs_[arc_index].is_final());
        output_accumulator_.push(arc_output(arcs_[arc_index].output()));
        target_upto = 0;

        size_t last_frame_index = 1;
        debug_check(valid_index_prefix_ <= term_.length());

        const size_t target_limit = std::min(target.size(), valid_index_prefix_);
        int32_t cmp = 0;

        while (target_upto < target_limit) {
            const int32_t term_byte = static_cast<int32_t>(term_.byte_at(target_upto));
            const int32_t target_byte = static_cast<int32_t>(target[target_upto]);
            cmp = term_byte - target_byte;
            if (cmp != 0) {
                break;
            }
            arc_index = 1 + target_upto;
            debug_check(arcs_[arc_index].label() == target_byte);
            output_accumulator_.push(arc_output(arcs_[arc_index].output()));

            if (arcs_[arc_index].is_final()) {
                last_frame_index = static_cast<size_t>(stack_[last_frame_index].ord + 2);
            }
            target_upto += 1;
        }

        if (cmp == 0) {
            const std::span<const uint8_t> current = term_.get_bytes_ref().as_slice();
            cmp = compare_spans(current.subspan(target_upto), target.subspan(target_upto));
        }

        if (cmp < 0) {
            current_frame_idx_ = last_frame_index;
        } else if (cmp > 0) {
            target_before_current_length_ = 0;
            if (auto rewound = stack_[last_frame_index].rewind(); !rewound) {
                return std::unexpected(std::move(rewound.error()));
            }
            current_frame_idx_ = last_frame_index;
        } else {
            debug_check(term_.length() == target.size());
            if (term_exists_) {
                return SeekStatus::Found;
            }
        }
    } else {
        target_before_current_length_ = -1;
        arc_index = 0;
        index->get_first_arc(arcs_[0]);
        debug_check(arcs_[0].is_final());
        output_accumulator_.push(arc_output(arcs_[0].output()));
        current_frame_idx_ = STATIC_FRAME_IDX;

        target_upto = 0;
        output_accumulator_.push(arc_output(arcs_[0].next_final_output()));
        const BytesRef next_final_output = arc_output(arcs_[0].next_final_output());
        auto pushed = push_frame_with_length(0, 0);
        if (!pushed) {
            return std::unexpected(std::move(pushed.error()));
        }
        current_frame_idx_ = pushed.value();
        output_accumulator_.pop(next_final_output);
    }

    while (target_upto < target.size()) {
        const int32_t target_label = static_cast<int32_t>(target[target_upto]);

        auto next_arc_idx_result = get_arc(1 + target_upto);
        if (!next_arc_idx_result) {
            return std::unexpected(std::move(next_arc_idx_result.error()));
        }
        const size_t next_arc_idx = next_arc_idx_result.value();

        debug_check(arc_index < next_arc_idx);
        auto found = index->find_target_arc(target_label, &arcs_[arc_index], arcs_[next_arc_idx], index->reader());
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }

        if (!found.value()) {
            valid_index_prefix_ = stack_[current_frame_idx_].prefix_length;
            if (auto scanned =
                        SegmentTermsEnumFrame::scan_to_floor_frame_with_target(current_frame_idx_, target, *this, true);
                !scanned) {
                return std::unexpected(std::move(scanned.error()));
            }
            auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
            if (!loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            auto result = SegmentTermsEnumFrame::scan_to_term(current_frame_idx_, target, false, *this);
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            if (result.value() == SeekStatus::End) {
                if (auto copied = term_.copy_bytes(target, 0, target.size()); !copied) {
                    return std::unexpected(std::move(copied.error()));
                }
                term_exists_ = false;
                auto following = next();
                if (!following) {
                    return std::unexpected(std::move(following.error()));
                }
                return following.value() != nullptr ? SeekStatus::NotFound : SeekStatus::End;
            }
            return result.value();
        }

        arc_index = next_arc_idx;
        term_.set_byte_at(target_upto, static_cast<uint8_t>(target_label));
        output_accumulator_.push(arc_output(arcs_[arc_index].output()));

        target_upto += 1;
        if (arcs_[arc_index].is_final()) {
            const BytesRef next_final_output = arc_output(arcs_[arc_index].next_final_output());
            output_accumulator_.push(next_final_output);
            auto pushed = push_frame_with_length(arc_index, target_upto);
            if (!pushed) {
                return std::unexpected(std::move(pushed.error()));
            }
            current_frame_idx_ = pushed.value();
            output_accumulator_.pop(next_final_output);
        }
    }

    valid_index_prefix_ = stack_[current_frame_idx_].prefix_length;
    if (auto scanned = SegmentTermsEnumFrame::scan_to_floor_frame_with_target(current_frame_idx_, target, *this, true);
        !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    auto loaded = SegmentTermsEnumFrame::load_block(current_frame_idx_, *this);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    auto result = SegmentTermsEnumFrame::scan_to_term(current_frame_idx_, target, false, *this);
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    if (result.value() != SeekStatus::End) {
        return result.value();
    }

    if (auto copied = term_.copy_bytes(target, 0, target.size()); !copied) {
        return std::unexpected(std::move(copied.error()));
    }
    term_exists_ = false;
    auto following = next();
    if (!following) {
        return std::unexpected(std::move(following.error()));
    }
    return following.value() != nullptr ? SeekStatus::NotFound : SeekStatus::End;
}

VoidResult SegmentTermsEnum::seek_exact_with_ord(int64_t /*ord*/) {
    return err(LuceneError::unsupported_operation("SegmentTermsEnum::seek_exact_with_ord"));
}

VoidResult SegmentTermsEnum::seek_exact_with_state(std::span<const uint8_t> target, const TermStateEnum &other_state) {
    eof_ = false;
    const int32_t cmp = term_exists_ ? compare_spans(target, term_.get_bytes_ref().as_slice()) : -1;
    if (cmp != 0 || !term_exists_) {
        stack_[STATIC_FRAME_IDX].state = other_state;
        current_frame_idx_ = STATIC_FRAME_IDX;
        if (auto copied = term_.copy_bytes(target, 0, target.size()); !copied) {
            return copied;
        }
        auto ord = stack_[STATIC_FRAME_IDX].get_term_block_ord();
        if (!ord) {
            return std::unexpected(std::move(ord.error()));
        }
        stack_[STATIC_FRAME_IDX].meta_data_upto = ord.value();
        debug_check(stack_[STATIC_FRAME_IDX].meta_data_upto > 0);
        valid_index_prefix_ = 0;
    }
    return {};
}

Result<const BytesRef *> SegmentTermsEnum::term() const {
    debug_check(!eof_);
    return &term_.get_bytes_ref();
}

Result<int64_t> SegmentTermsEnum::ord() const {
    return err(LuceneError::unsupported_operation("SegmentTermsEnum has no ordinals"));
}

Result<AttributeSource *> SegmentTermsEnum::attributes() {
    return err(LuceneError::unsupported_operation("SegmentTermsEnum has no attributes"));
}

Result<AttributeSource *> SegmentTermsEnum::attributes_mut() {
    return err(LuceneError::unsupported_operation("SegmentTermsEnum has no attributes"));
}

Result<int32_t> SegmentTermsEnum::doc_freq() {
    debug_check(!eof_);
    if (auto decoded = SegmentTermsEnumFrame::decode_meta_data(current_frame_idx_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto block_state = stack_[current_frame_idx_].state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return block_state.value()->doc_freq;
}

Result<int64_t> SegmentTermsEnum::total_term_freq() {
    debug_check(!eof_);
    if (auto decoded = SegmentTermsEnumFrame::decode_meta_data(current_frame_idx_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    auto block_state = stack_[current_frame_idx_].state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return block_state.value()->total_term_freq;
}

Result<std::unique_ptr<PostingsEnum>> SegmentTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) {
    debug_check(!eof_);
    if (auto decoded = SegmentTermsEnumFrame::decode_meta_data(current_frame_idx_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    if (fr_->parent() == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    return fr_->parent()->postings_reader().postings(*fr_->field_info(), stack_[current_frame_idx_].state,
                                                     std::move(reuse), flags);
}

Result<std::unique_ptr<ImpactsEnum>> SegmentTermsEnum::impacts(int32_t flags) {
    debug_check(!eof_);
    if (auto decoded = SegmentTermsEnumFrame::decode_meta_data(current_frame_idx_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    if (fr_->parent() == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    return fr_->parent()->postings_reader().impacts(*fr_->field_info(), stack_[current_frame_idx_].state, flags);
}

Result<TermStateEnum> SegmentTermsEnum::term_state() {
    debug_check(!eof_);
    if (auto decoded = SegmentTermsEnumFrame::decode_meta_data(current_frame_idx_, *this); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return stack_[current_frame_idx_].state;
}

std::string SegmentTermsEnum::to_string() const {
    return std::format("SegmentTermsEnum(field={},numTerms={},frame={}/{})", fr_->field_info()->name,
                       fr_->num_terms_, current_frame_idx_, stack_.size());
}

// ---- SegmentTermsEnumFrame ----


namespace {

/// Lexicographic comparison with unsigned bytes, matching Rust's `Ord` on
/// `&[u8]`.
int32_t compare_bytes(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    const size_t shared = std::min(left.size(), right.size());
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

} // namespace

Result<SegmentTermsEnumFrame> SegmentTermsEnumFrame::create(int32_t ord, FieldReader &fr) {
    if (fr.parent() == nullptr) {
        return err(LuceneError::illegal_state("block-tree terms reader is missing"));
    }
    SegmentTermsEnumFrame frame;
    auto state = fr.parent()->postings_reader().new_term_state();
    if (!state) {
        return std::unexpected(std::move(state.error()));
    }
    frame.state = std::move(state.value());
    auto block = frame.state.get_block_term_state_mut();
    if (!block) {
        return std::unexpected(std::move(block.error()));
    }
    (*block)->total_term_freq = -1;

    frame.ord = ord;
    frame.arc.reset();
    frame.has_terms = false;
    frame.has_terms_orig = false;
    frame.is_floor = false;

    // The reference implementation pre-sizes the buffers the same way; they grow on demand.
    frame.suffix_bytes.assign(32, 0);
    frame.suffix_length_bytes.assign(32, 0);
    frame.stats_bytes.assign(64, 0);
    frame.metadata_bytes.assign(32, 0);
    frame.reseat_readers();

    frame.prefix_length = 0;
    frame.ent_count = 0;
    frame.next_ent = 0;
    frame.is_last_in_floor = false;
    frame.is_leaf_block = false;
    frame.all_equal = false;
    frame.last_sub_fp = 0;
    frame.next_floor_label = 0;
    frame.num_follow_floor_blocks = 0;
    frame.meta_data_upto = 0;
    frame.start_byte_pos = 0;
    frame.suffix_length = 0;
    frame.sub_code = 0;
    frame.compression_alg = CompressionAlgorithm::NoCompression;
    return frame;
}

void SegmentTermsEnumFrame::reseat_readers() {
    suffixes_reader.reset_with_range(std::span<const uint8_t>(suffix_bytes), 0, 0);
    suffix_lengths_reader.reset_with_range(std::span<const uint8_t>(suffix_length_bytes), 0, 0);
    stats_reader.reset_with_range(std::span<const uint8_t>(stats_bytes), 0, 0);
    bytes_reader.reset_with_range(std::span<const uint8_t>(metadata_bytes), 0, 0);
    floor_data_reader.reset_with_range(std::span<const uint8_t>(floor_bytes), 0, 0);
}

VoidResult SegmentTermsEnumFrame::set_floor_data(const OutputAccumulator &output_accumulator) {
    if (auto copied = output_accumulator.set_floor_data(floor_bytes); !copied) {
        return copied;
    }
    floor_data_reader.reset_with_range(std::span<const uint8_t>(floor_bytes), 0, floor_bytes.size());
    rewind_pos = floor_data_reader.get_position();

    auto follow_blocks = floor_data_reader.read_vint();
    if (!follow_blocks) {
        return std::unexpected(std::move(follow_blocks.error()));
    }
    num_follow_floor_blocks = follow_blocks.value();
    auto label = floor_data_reader.read_byte();
    if (!label) {
        return std::unexpected(std::move(label.error()));
    }
    next_floor_label = static_cast<int32_t>(label.value());
    return {};
}

Result<int32_t> SegmentTermsEnumFrame::get_term_block_ord() const {
    if (is_leaf_block) {
        return next_ent;
    }
    auto block_state = state.get_block_term_state();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    return block_state.value()->term_block_ord;
}

Result<bool> SegmentTermsEnumFrame::load_next_floor_block(size_t frame_idx, SegmentTermsEnum &ste) {
    auto &frame = ste.stack()[frame_idx];
    debug_check(!frame.arc.has_value() || frame.is_floor);

    frame.fp = frame.fp_end;
    frame.next_ent = -1;
    return load_block(frame_idx, ste);
}

VoidResult SegmentTermsEnumFrame::prefetch_block(size_t frame_idx, SegmentTermsEnum &ste) {
    if (ste.stack()[frame_idx].next_ent != -1) {
        // Already loaded.
        return {};
    }
    const size_t fp = ste.stack()[frame_idx].fp;
    if (auto initialized = ste.init_index_input(); !initialized) {
        return initialized;
    }
    if (ste.input() == nullptr) {
        return err(LuceneError::illegal_state("segment terms input is not initialized"));
    }
    return ste.input()->prefetch(fp, 1);
}

Result<bool> SegmentTermsEnumFrame::load_block(size_t frame_index, SegmentTermsEnum &ste) {
    if (auto initialized = ste.init_index_input(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    auto &frame = ste.stack()[frame_index];
    if (frame.next_ent != -1) {
        return frame.is_leaf_block; // already loaded
    }
    IndexInput *input = ste.input().get();
    if (input == nullptr) {
        return err(LuceneError::illegal_state("segment terms input is not initialized"));
    }

    if (auto seeked = input->seek(frame.fp); !seeked) {
        return std::unexpected(std::move(seeked.error()));
    }
    auto code = input->read_vint();
    if (!code) {
        return std::unexpected(std::move(code.error()));
    }
    frame.ent_count = static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
    debug_check(frame.ent_count > 0);
    frame.is_last_in_floor = (code.value() & 1) != 0;
    debug_check(!frame.arc.has_value() || frame.is_last_in_floor || frame.is_floor);

    auto start_suffix_fp = input->get_file_pointer();
    if (!start_suffix_fp) {
        return std::unexpected(std::move(start_suffix_fp.error()));
    }

    // Term suffixes.
    auto code_l = input->read_vlong();
    if (!code_l) {
        return std::unexpected(std::move(code_l.error()));
    }
    frame.is_leaf_block = (code_l.value() & 0x04) != 0;
    const int32_t num_suffix_bytes = static_cast<int32_t>(static_cast<uint64_t>(code_l.value()) >> 3);
    if (num_suffix_bytes < 0) {
        return err(LuceneError::corrupt_index("negative suffix length"));
    }
    const size_t suffix_length = static_cast<size_t>(num_suffix_bytes);
    if (frame.suffix_bytes.size() < suffix_length) {
        frame.suffix_bytes.resize(suffix_length, 0);
    }

    auto alg = compression_algorithm_by_code(static_cast<uint8_t>(code_l.value() & 0x03));
    if (!alg) {
        return err(LuceneError::corrupt_index(
                std::format("{} (resource={})", alg.error().message(), input->to_string())));
    }
    frame.compression_alg = alg.value();
    if (auto read = read_compressed(frame.compression_alg, *input, std::span<uint8_t>(frame.suffix_bytes),
                                    num_suffix_bytes);
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    frame.suffixes_reader.reset_with_range(std::span<const uint8_t>(frame.suffix_bytes), 0, suffix_length);

    auto suffix_length_bytes = input->read_vint();
    if (!suffix_length_bytes) {
        return std::unexpected(std::move(suffix_length_bytes.error()));
    }
    if (suffix_length_bytes.value() < 0) {
        return err(LuceneError::corrupt_index("negative suffix length block size"));
    }
    size_t num_suffix_length_bytes = static_cast<size_t>(suffix_length_bytes.value());
    frame.all_equal = (num_suffix_length_bytes & 0x01) != 0;
    num_suffix_length_bytes >>= 1;
    if (frame.suffix_length_bytes.size() < num_suffix_length_bytes) {
        frame.suffix_length_bytes.resize(num_suffix_length_bytes, 0);
    }
    if (frame.all_equal) {
        auto fill_byte = input->read_byte();
        if (!fill_byte) {
            return std::unexpected(std::move(fill_byte.error()));
        }
        std::fill_n(frame.suffix_length_bytes.begin(), static_cast<std::ptrdiff_t>(num_suffix_length_bytes),
                    fill_byte.value());
    } else {
        const std::span<uint8_t> target(frame.suffix_length_bytes);
        if (auto read = input->read_bytes(target.first(num_suffix_length_bytes)); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    frame.suffix_lengths_reader.reset_with_range(std::span<const uint8_t>(frame.suffix_length_bytes), 0,
                                                 num_suffix_length_bytes);

    auto after_suffix = input->get_file_pointer();
    if (!after_suffix) {
        return std::unexpected(std::move(after_suffix.error()));
    }
    frame.total_suffix_bytes = static_cast<int64_t>(after_suffix.value()) -
                               static_cast<int64_t>(start_suffix_fp.value());

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

    auto block_state = frame.state.get_block_term_state_mut();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    (*block_state)->term_block_ord = 0;
    frame.next_ent = 0;
    frame.last_sub_fp.reset();

    // Metadata: kept as a blob and decoded lazily.
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

    auto fp_end = input->get_file_pointer();
    if (!fp_end) {
        return std::unexpected(std::move(fp_end.error()));
    }
    frame.fp_end = fp_end.value();

    return frame.is_leaf_block;
}

VoidResult SegmentTermsEnumFrame::rewind() {
    // Force a reload.
    fp = fp_orig;
    next_ent = -1;
    has_terms = has_terms_orig;

    if (is_floor) {
        floor_data_reader.set_position(rewind_pos);
        auto follow_blocks = floor_data_reader.read_vint();
        if (!follow_blocks) {
            return std::unexpected(std::move(follow_blocks.error()));
        }
        num_follow_floor_blocks = follow_blocks.value();
        debug_check(num_follow_floor_blocks > 0);
        auto label = floor_data_reader.read_byte();
        if (!label) {
            return std::unexpected(std::move(label.error()));
        }
        next_floor_label = static_cast<int32_t>(label.value());
    }
    return {};
}

Result<bool> SegmentTermsEnumFrame::next(size_t frame_idx, SegmentTermsEnum &ste) {
    if (ste.stack()[frame_idx].is_leaf_block) {
        if (auto advanced = next_leaf(frame_idx, ste); !advanced) {
            return std::unexpected(std::move(advanced.error()));
        }
        return false;
    }
    return next_non_leaf(frame_idx, ste);
}

VoidResult SegmentTermsEnumFrame::next_leaf(size_t frame_idx, SegmentTermsEnum &ste) {
    auto &frame = ste.stack()[frame_idx];
    debug_check(frame.next_ent != -1 && frame.next_ent < frame.ent_count);

    frame.next_ent += 1;
    auto suffix_length = frame.suffix_lengths_reader.read_vint();
    if (!suffix_length) {
        return std::unexpected(std::move(suffix_length.error()));
    }
    if (suffix_length.value() < 0) {
        return err(LuceneError::corrupt_index("negative suffix length"));
    }
    frame.suffix_length = static_cast<size_t>(suffix_length.value());
    frame.start_byte_pos = frame.suffixes_reader.get_position();
    // The suffix bytes are read below through `suffix_bytes`; advance the reader
    // so that the next entry starts where this one ends.
    if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix_length)); !skipped) {
        return skipped;
    }

    const size_t term_len = frame.prefix_length + frame.suffix_length;
    auto &term = ste.term_builder();
    term.set_length(term_len);
    if (auto grown = term.grow(term_len); !grown) {
        return grown;
    }
    const std::span<const uint8_t> suffix =
            std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
    std::span<uint8_t> dest = term.bytes_mut().as_mutable_slice();
    std::copy(suffix.begin(), suffix.end(), dest.begin() + static_cast<std::ptrdiff_t>(frame.prefix_length));

    ste.term_exists() = true;
    return {};
}

Result<bool> SegmentTermsEnumFrame::next_non_leaf(size_t frame_idx, SegmentTermsEnum &ste) {
    while (true) {
        if (ste.stack()[frame_idx].next_ent == ste.stack()[frame_idx].ent_count) {
            debug_check(!ste.stack()[frame_idx].arc.has_value() ||
                        (ste.stack()[frame_idx].is_floor && !ste.stack()[frame_idx].is_last_in_floor));

            auto is_leaf_block = load_next_floor_block(frame_idx, ste);
            if (!is_leaf_block) {
                return std::unexpected(std::move(is_leaf_block.error()));
            }
            if (is_leaf_block.value()) {
                if (auto advanced = next_leaf(frame_idx, ste); !advanced) {
                    return std::unexpected(std::move(advanced.error()));
                }
                return false;
            }
            continue;
        }

        auto &frame = ste.stack()[frame_idx];
        debug_check(frame.next_ent != -1 && frame.next_ent < frame.ent_count);

        frame.next_ent += 1;

        auto code = frame.suffix_lengths_reader.read_vint();
        if (!code) {
            return std::unexpected(std::move(code.error()));
        }
        if (code.value() < 0) {
            return err(LuceneError::corrupt_index("negative suffix length"));
        }
        frame.suffix_length = static_cast<size_t>(static_cast<uint32_t>(code.value()) >> 1);
        frame.start_byte_pos = frame.suffixes_reader.get_position();
        // See `next_leaf`: the slice below is read from `suffix_bytes`, so the
        // reader still has to move past the suffix.
        if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix_length)); !skipped) {
            return std::unexpected(std::move(skipped.error()));
        }

        const size_t term_len = frame.prefix_length + frame.suffix_length;
        auto &term = ste.term_builder();
        term.set_length(term_len);
        if (auto grown = term.grow(term_len); !grown) {
            return std::unexpected(std::move(grown.error()));
        }
        const std::span<const uint8_t> suffix =
                std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
        std::span<uint8_t> dest = term.bytes_mut().as_mutable_slice();
        std::copy(suffix.begin(), suffix.end(), dest.begin() + static_cast<std::ptrdiff_t>(frame.prefix_length));

        if ((code.value() & 1) == 0) {
            // Normal term.
            ste.term_exists() = true;
            frame.sub_code = 0;
            auto block_state = frame.state.get_block_term_state_mut();
            if (!block_state) {
                return std::unexpected(std::move(block_state.error()));
            }
            (*block_state)->term_block_ord += 1;
            return false;
        }
        // A sub-block: make the sub-FP absolute.
        ste.term_exists() = false;
        auto sub_code = frame.suffix_lengths_reader.read_vlong();
        if (!sub_code) {
            return std::unexpected(std::move(sub_code.error()));
        }
        frame.sub_code = sub_code.value();
        frame.last_sub_fp = static_cast<size_t>(static_cast<int64_t>(frame.fp) - frame.sub_code);
        return true;
    }
}

VoidResult SegmentTermsEnumFrame::scan_to_floor_frame(size_t frame_idx, SegmentTermsEnum &ste) {
    static const BytesRef kEmpty{};
    return scan_to_floor_frame_with_target(frame_idx, kEmpty.as_slice(), ste, false);
}

VoidResult SegmentTermsEnumFrame::scan_to_floor_frame_with_target(size_t frame_idx, std::span<const uint8_t> target,
                                                                 SegmentTermsEnum &ste, bool use_target) {
    auto &frame = ste.stack()[frame_idx];
    const std::span<const uint8_t> target_bytes = use_target ? target : ste.term_bytes();
    if (!frame.is_floor || target_bytes.size() <= frame.prefix_length) {
        return {};
    }

    const int32_t target_label = static_cast<int32_t>(target_bytes[frame.prefix_length]);
    if (target_label < frame.next_floor_label) {
        return {};
    }

    debug_check(frame.num_follow_floor_blocks != 0);

    size_t new_fp = 0;
    while (true) {
        auto code = frame.floor_data_reader.read_vlong();
        if (!code) {
            return std::unexpected(std::move(code.error()));
        }
        new_fp = frame.fp_orig +
                 static_cast<size_t>(static_cast<uint64_t>(code.value()) >> 1);
        frame.has_terms = (code.value() & 1) != 0;

        frame.is_last_in_floor = frame.num_follow_floor_blocks == 1;
        frame.num_follow_floor_blocks -= 1;

        if (frame.is_last_in_floor) {
            frame.next_floor_label = 256;
            break;
        }
        auto label = frame.floor_data_reader.read_byte();
        if (!label) {
            return std::unexpected(std::move(label.error()));
        }
        frame.next_floor_label = static_cast<int32_t>(label.value());
        if (target_label < frame.next_floor_label) {
            break;
        }
    }

    if (new_fp != frame.fp) {
        frame.next_ent = -1;
        frame.fp = new_fp;
    }
    return {};
}

VoidResult SegmentTermsEnumFrame::decode_meta_data(size_t frame_idx, SegmentTermsEnum &ste) {
    auto &frame = ste.stack()[frame_idx];
    auto limit_result = frame.get_term_block_ord();
    if (!limit_result) {
        return std::unexpected(std::move(limit_result.error()));
    }
    const int32_t limit = limit_result.value();
    bool absolute = frame.meta_data_upto == 0;
    debug_check(limit > 0);

    while (frame.meta_data_upto < limit) {
        auto block_state = frame.state.get_block_term_state_mut();
        if (!block_state) {
            return std::unexpected(std::move(block_state.error()));
        }

        if (frame.stats_singleton_run_length > 0) {
            (*block_state)->doc_freq = 1;
            (*block_state)->total_term_freq = 1;
            frame.stats_singleton_run_length -= 1;
        } else {
            auto token = frame.stats_reader.read_vint();
            if (!token) {
                return std::unexpected(std::move(token.error()));
            }
            if ((token.value() & 1) == 1) {
                (*block_state)->doc_freq = 1;
                (*block_state)->total_term_freq = 1;
                frame.stats_singleton_run_length = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 1);
            } else {
                (*block_state)->doc_freq = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 1);
                if (ste.fr().field_info()->index_options == IndexOptions::Docs) {
                    (*block_state)->total_term_freq = (*block_state)->doc_freq;
                } else {
                    auto extra = frame.stats_reader.read_vlong();
                    if (!extra) {
                        return std::unexpected(std::move(extra.error()));
                    }
                    (*block_state)->total_term_freq = static_cast<int64_t>((*block_state)->doc_freq) + extra.value();
                }
            }
        }

        if (ste.fr().parent() == nullptr) {
            return err(LuceneError::illegal_state("block-tree terms reader is missing"));
        }
        if (auto decoded = ste.fr().parent()->postings_reader().decode_term(
                    frame.bytes_reader, *ste.fr().field_info(), frame.state, absolute);
            !decoded) {
            return decoded;
        }

        frame.meta_data_upto += 1;
        absolute = false;
    }

    auto block_state = frame.state.get_block_term_state_mut();
    if (!block_state) {
        return std::unexpected(std::move(block_state.error()));
    }
    (*block_state)->term_block_ord = frame.meta_data_upto;
    return {};
}

bool SegmentTermsEnumFrame::prefix_matches(size_t frame_idx, std::span<const uint8_t> target,
                                           const SegmentTermsEnum &ste) {
    const auto &frame = ste.stack()[frame_idx];
    // Lucene compares the raw term buffer: while the index is walked the term
    // length is not updated yet, but the bytes written so far are the prefix to
    // compare against.
    const std::vector<uint8_t> &term = ste.term_builder().get_bytes_ref().bytes();
    for (size_t byte_pos = 0; byte_pos < frame.prefix_length; ++byte_pos) {
        if (byte_pos >= target.size() || byte_pos >= term.size() || target[byte_pos] != term[byte_pos]) {
            return false;
        }
    }
    return true;
}

VoidResult SegmentTermsEnumFrame::scan_to_sub_block(size_t frame_idx, size_t sub_fp, SegmentTermsEnum &ste) {
    auto &frame = ste.stack()[frame_idx];
    debug_check(!frame.is_leaf_block);
    if (frame.last_sub_fp.has_value() && frame.last_sub_fp.value() == sub_fp) {
        return {};
    }

    debug_check(sub_fp < frame.fp);
    const int64_t target_sub_code = static_cast<int64_t>(frame.fp) - static_cast<int64_t>(sub_fp);

    while (true) {
        debug_check(frame.next_ent < frame.ent_count);
        frame.next_ent += 1;

        auto code = frame.suffix_lengths_reader.read_vint();
        if (!code) {
            return std::unexpected(std::move(code.error()));
        }
        if (auto skipped = frame.suffixes_reader.skip_bytes(
                    static_cast<int64_t>(static_cast<uint64_t>(code.value()) >> 1));
            !skipped) {
            return skipped;
        }

        if ((code.value() & 1) != 0) {
            auto sub_code = frame.suffix_lengths_reader.read_vlong();
            if (!sub_code) {
                return std::unexpected(std::move(sub_code.error()));
            }
            if (target_sub_code == sub_code.value()) {
                frame.last_sub_fp = sub_fp;
                return {};
            }
        } else {
            auto block_state = frame.state.get_block_term_state_mut();
            if (!block_state) {
                return std::unexpected(std::move(block_state.error()));
            }
            (*block_state)->term_block_ord += 1;
        }
    }
}

Result<SeekStatus> SegmentTermsEnumFrame::scan_to_term(size_t frame_idx, std::span<const uint8_t> target,
                                                       bool exact_only, SegmentTermsEnum &ste) {
    if (ste.stack()[frame_idx].is_leaf_block) {
        if (ste.stack()[frame_idx].all_equal) {
            return binary_search_term_leaf(frame_idx, target, exact_only, ste);
        }
        return scan_to_term_leaf(frame_idx, target, exact_only, ste);
    }
    return scan_to_term_non_leaf(frame_idx, target, exact_only, ste);
}

Result<SeekStatus> SegmentTermsEnumFrame::scan_to_term_leaf(size_t frame_idx, std::span<const uint8_t> target,
                                                            bool exact_only, SegmentTermsEnum &ste) {
    {
        auto &frame = ste.stack()[frame_idx];
        debug_check(frame.next_ent != -1);
        ste.term_exists() = true;
        frame.sub_code = 0;

        if (frame.next_ent == frame.ent_count) {
            if (exact_only) {
                if (auto filled = fill_term(frame_idx, ste); !filled) {
                    return std::unexpected(std::move(filled.error()));
                }
            }
            return SeekStatus::End;
        }
    }

    debug_check(prefix_matches(frame_idx, target, ste));

    while (true) {
        auto &frame = ste.stack()[frame_idx];
        frame.next_ent += 1;
        auto suffix_length = frame.suffix_lengths_reader.read_vint();
        if (!suffix_length) {
            return std::unexpected(std::move(suffix_length.error()));
        }
        if (suffix_length.value() < 0) {
            return err(LuceneError::corrupt_index("negative suffix length"));
        }
        frame.suffix_length = static_cast<size_t>(suffix_length.value());
        frame.start_byte_pos = frame.suffixes_reader.get_position();
        if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix_length)); !skipped) {
            return std::unexpected(std::move(skipped.error()));
        }

        const std::span<const uint8_t> suffix =
                std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
        const std::span<const uint8_t> target_suffix = target.subspan(frame.prefix_length);
        const int32_t cmp = compare_bytes(suffix, target_suffix);

        if (cmp < 0) {
            // Current entry is still before the target; keep scanning.
        } else if (cmp > 0) {
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            return SeekStatus::NotFound;
        } else {
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            return SeekStatus::Found;
        }
        if (frame.next_ent >= frame.ent_count) {
            break;
        }
    }

    // The index pointed us at this block but the term is not in it: the target
    // is after the last term of the block and before the next indexed term.
    if (exact_only) {
        if (auto filled = fill_term(frame_idx, ste); !filled) {
            return std::unexpected(std::move(filled.error()));
        }
    }
    return SeekStatus::End;
}

Result<SeekStatus> SegmentTermsEnumFrame::binary_search_term_leaf(size_t frame_idx, std::span<const uint8_t> target,
                                                                 bool exact_only, SegmentTermsEnum &ste) {
    {
        auto &frame = ste.stack()[frame_idx];
        debug_check(frame.next_ent != -1);
        ste.term_exists() = true;
        frame.sub_code = 0;

        if (frame.next_ent == frame.ent_count) {
            if (exact_only) {
                if (auto filled = fill_term(frame_idx, ste); !filled) {
                    return std::unexpected(std::move(filled.error()));
                }
            }
            return SeekStatus::End;
        }
    }

    debug_check(prefix_matches(frame_idx, target, ste));

    auto &frame = ste.stack()[frame_idx];
    auto suffix_length = frame.suffix_lengths_reader.read_vint();
    if (!suffix_length) {
        return std::unexpected(std::move(suffix_length.error()));
    }
    if (suffix_length.value() < 0) {
        return err(LuceneError::corrupt_index("negative suffix length"));
    }
    frame.suffix_length = static_cast<size_t>(suffix_length.value());

    int32_t start = frame.next_ent;
    int32_t end = frame.ent_count - 1;
    int32_t cmp = 0;

    const std::span<const uint8_t> target_suffix = target.subspan(frame.prefix_length);
    while (start <= end) {
        const int32_t mid = static_cast<int32_t>(static_cast<uint32_t>(start + end) >> 1);
        frame.next_ent = mid + 1;
        frame.start_byte_pos = static_cast<size_t>(mid) * frame.suffix_length;

        const std::span<const uint8_t> suffix =
                std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
        cmp = compare_bytes(suffix, target_suffix);

        if (cmp < 0) {
            start = mid + 1;
        } else if (cmp > 0) {
            end = mid - 1;
        } else {
            frame.suffixes_reader.set_position(frame.start_byte_pos + frame.suffix_length);
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            return SeekStatus::Found;
        }
    }

    SeekStatus seek_status = SeekStatus::NotFound;
    if (end < frame.ent_count - 1) {
        seek_status = SeekStatus::NotFound;
        if (cmp < 0) {
            frame.start_byte_pos += frame.suffix_length;
            frame.next_ent += 1;
        }
        frame.suffixes_reader.set_position(frame.start_byte_pos + frame.suffix_length);
        if (auto filled = fill_term(frame_idx, ste); !filled) {
            return std::unexpected(std::move(filled.error()));
        }
    } else {
        seek_status = SeekStatus::End;
        frame.suffixes_reader.set_position(frame.start_byte_pos + frame.suffix_length);
        if (exact_only) {
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
        }
    }
    return seek_status;
}

Result<SeekStatus> SegmentTermsEnumFrame::scan_to_term_non_leaf(size_t frame_idx, std::span<const uint8_t> target,
                                                               bool exact_only, SegmentTermsEnum &ste) {
    debug_check(ste.stack()[frame_idx].next_ent != -1);

    if (ste.stack()[frame_idx].next_ent == ste.stack()[frame_idx].ent_count) {
        if (exact_only) {
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            ste.term_exists() = ste.stack()[frame_idx].sub_code == 0;
        }
        return SeekStatus::End;
    }
    debug_check(prefix_matches(frame_idx, target, ste));

    while (ste.stack()[frame_idx].next_ent < ste.stack()[frame_idx].ent_count) {
        int32_t cmp = 0;
        {
            auto &frame = ste.stack()[frame_idx];
            frame.next_ent += 1;
            auto code = frame.suffix_lengths_reader.read_vint();
            if (!code) {
                return std::unexpected(std::move(code.error()));
            }
            if (code.value() < 0) {
                return err(LuceneError::corrupt_index("negative suffix length"));
            }
            frame.suffix_length = static_cast<size_t>(static_cast<uint32_t>(code.value()) >> 1);
            frame.start_byte_pos = frame.suffixes_reader.get_position();
            if (auto skipped = frame.suffixes_reader.skip_bytes(static_cast<int64_t>(frame.suffix_length));
                !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }

            const bool exists = (code.value() & 1) == 0;
            ste.term_exists() = exists;
            if (exists) {
                auto block_state = frame.state.get_block_term_state_mut();
                if (!block_state) {
                    return std::unexpected(std::move(block_state.error()));
                }
                (*block_state)->term_block_ord += 1;
                frame.sub_code = 0;
            } else {
                auto sub_code = frame.suffix_lengths_reader.read_vlong();
                if (!sub_code) {
                    return std::unexpected(std::move(sub_code.error()));
                }
                frame.sub_code = sub_code.value();
                frame.last_sub_fp = static_cast<size_t>(static_cast<int64_t>(frame.fp) - frame.sub_code);
            }

            const std::span<const uint8_t> suffix =
                    std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
            cmp = compare_bytes(suffix, target.subspan(frame.prefix_length));
        }

        if (cmp < 0) {
            // Current entry is still before the target; keep scanning.
        } else if (cmp > 0) {
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            if (!exact_only && !ste.term_exists()) {
                // We are on a sub-block and the caller wants the next term after
                // the target, so we must recurse into the sub-frames.
                auto last_sub_fp = ste.stack()[ste.current_frame_idx()].last_sub_fp;
                if (!last_sub_fp.has_value()) {
                    return err(LuceneError::illegal_state("term block has no current sub-block"));
                }
                const size_t prefix_len = ste.stack()[frame_idx].prefix_length +
                                          ste.stack()[frame_idx].suffix_length;
                auto current_frame_idx = ste.push_frame(last_sub_fp, last_sub_fp.value(), prefix_len);
                if (!current_frame_idx) {
                    return std::unexpected(std::move(current_frame_idx.error()));
                }
                ste.current_frame_idx() = current_frame_idx.value();

                auto loaded = load_block(current_frame_idx.value(), ste);
                if (!loaded) {
                    return std::unexpected(std::move(loaded.error()));
                }
                while (true) {
                    auto more = next(current_frame_idx.value(), ste);
                    if (!more) {
                        return std::unexpected(std::move(more.error()));
                    }
                    if (!more.value()) {
                        break;
                    }
                    auto deeper_sub_fp = ste.stack()[ste.current_frame_idx()].last_sub_fp;
                    if (!deeper_sub_fp.has_value()) {
                        return err(LuceneError::illegal_state("term block has no current sub-block"));
                    }
                    const size_t next_prefix = ste.term_bytes().size();
                    auto deeper = ste.push_frame(deeper_sub_fp, deeper_sub_fp.value(), next_prefix);
                    if (!deeper) {
                        return std::unexpected(std::move(deeper.error()));
                    }
                    current_frame_idx = deeper.value();
                    ste.current_frame_idx() = current_frame_idx.value();
                    auto deeper_loaded = load_block(current_frame_idx.value(), ste);
                    if (!deeper_loaded) {
                        return std::unexpected(std::move(deeper_loaded.error()));
                    }
                }
            }
            return SeekStatus::NotFound;
        } else {
            debug_check(ste.term_exists());
            if (auto filled = fill_term(frame_idx, ste); !filled) {
                return std::unexpected(std::move(filled.error()));
            }
            return SeekStatus::Found;
        }
    }

    if (exact_only) {
        if (auto filled = fill_term(frame_idx, ste); !filled) {
            return std::unexpected(std::move(filled.error()));
        }
    }
    return SeekStatus::End;
}

VoidResult SegmentTermsEnumFrame::fill_term(size_t frame_idx, SegmentTermsEnum &ste) {
    auto &frame = ste.stack()[frame_idx];
    const size_t term_length = frame.prefix_length + frame.suffix_length;
    auto &term = ste.term_builder();
    term.set_length(term_length);
    if (auto grown = term.grow(term_length); !grown) {
        return grown;
    }
    const std::span<const uint8_t> src =
            std::span<const uint8_t>(frame.suffix_bytes).subspan(frame.start_byte_pos, frame.suffix_length);
    std::span<uint8_t> dest = term.bytes_mut().as_mutable_slice();
    std::copy(src.begin(), src.end(), dest.begin() + static_cast<std::ptrdiff_t>(frame.prefix_length));
    return {};
}

// ---- FieldReader ----


Result<std::unique_ptr<FieldReader>> FieldReader::create(std::shared_ptr<const FieldInfo> field_info,
                                                         int64_t num_terms, BytesRef root_code,
                                                         int64_t sum_total_term_freq, int64_t sum_doc_freq,
                                                         int32_t doc_count, int64_t index_start_fp,
                                                         DataInput &meta_in, BytesRef min_term, BytesRef max_term) {
    debug_check(num_terms > 0);

    auto metadata = FSTMetadata::read(meta_in, FstByteSequenceOutputs::singleton());
    if (!metadata) {
        return std::unexpected(std::move(metadata.error()));
    }

    auto reader = std::unique_ptr<FieldReader>(new FieldReader());
    reader->field_info_ = std::move(field_info);
    reader->num_terms_ = num_terms;
    reader->sum_total_term_freq_ = sum_total_term_freq;
    reader->sum_doc_freq_ = sum_doc_freq;
    reader->doc_count_ = doc_count;
    // Padding values, replaced by `init_field_reader`.
    reader->root_block_fp_ = 0;
    reader->root_code_ = BytesRef{};
    reader->min_term_ = std::move(min_term);
    reader->max_term_ = std::move(max_term);
    reader->pending_metadata_ = std::move(metadata.value());
    reader->pending_index_start_fp_ = index_start_fp;
    reader->pending_root_code_ = std::move(root_code);
    return reader;
}

VoidResult FieldReader::init_field_reader(IndexInput &index_in, FieldReader &reader) {
    if (!reader.pending_metadata_.has_value() || !reader.pending_index_start_fp_.has_value()) {
        return err(LuceneError::illegal_state("TmpData is None, cannot init FieldReader"));
    }
    FSTMetadata metadata = std::move(reader.pending_metadata_.value());
    reader.pending_metadata_.reset();
    const int64_t index_start_fp = reader.pending_index_start_fp_.value();
    reader.pending_index_start_fp_.reset();

    const int64_t num_bytes = metadata.num_bytes;
    if (num_bytes < 0) {
        return err(LuceneError::corrupt_index(std::format("negative FST length: {}", num_bytes)));
    }
    std::vector<uint8_t> fst_bytes(static_cast<size_t>(num_bytes), 0);
    if (auto seeked = index_in.seek(static_cast<size_t>(index_start_fp)); !seeked) {
        return seeked;
    }
    if (auto read = index_in.read_bytes(fst_bytes); !read) {
        return read;
    }

    // Keep the FST's empty output: the root code is replaced by it when equal,
    // so that the root block's floor data can be read from the FST.
    const std::optional<std::any> empty_output = metadata.empty_output;

    const OnHeapFstStore store(std::move(fst_bytes));
    auto fst_reader = store.make_reader();
    if (fst_reader == nullptr) {
        return err(LuceneError::illegal_state("FST metadata and store are some, should not return None"));
    }
    reader.index_ = std::make_shared<FST>(std::move(metadata), std::move(fst_reader));

    BytesRef root_code = std::move(reader.pending_root_code_);
    const std::span<const uint8_t> root_bytes = root_code.as_slice();
    ByteArrayDataInput input{root_bytes};
    auto root_output = read_msb_vlong(input);
    if (!root_output) {
        return std::unexpected(std::move(root_output.error()));
    }
    const uint64_t output = static_cast<uint64_t>(root_output.value());
    reader.root_block_fp_ = static_cast<int64_t>(output >> OUTPUT_FLAG_HAS_TERMS);

    bool replaced = false;
    if (empty_output.has_value()) {
        const auto *empty = std::any_cast<BytesRef>(&empty_output.value());
        if (empty != nullptr && *empty == root_code) {
            reader.root_code_ = *empty;
            replaced = true;
        }
    }
    if (!replaced) {
        reader.root_code_ = std::move(root_code);
    }
    return {};
}

Result<std::unique_ptr<FieldReader>> FieldReader::clone_for_enum() const {
    if (!index_) {
        return err(LuceneError::illegal_state("FieldReader is not initialized"));
    }
    auto copy = std::unique_ptr<FieldReader>(new FieldReader());
    copy->num_terms_ = num_terms_;
    copy->field_info_ = field_info_;
    copy->sum_total_term_freq_ = sum_total_term_freq_;
    copy->sum_doc_freq_ = sum_doc_freq_;
    copy->doc_count_ = doc_count_;
    copy->root_block_fp_ = root_block_fp_;
    copy->root_code_ = root_code_;
    copy->min_term_ = min_term_;
    copy->max_term_ = max_term_;
    // `Arc::clone`: the FST is shared, not duplicated.
    copy->parent_ = parent_;
    copy->index_ = index_;
    return copy;
}

Result<std::unique_ptr<TermsEnum>> FieldReader::iterator() const {
    auto copy = clone_for_enum();
    if (!copy) {
        return std::unexpected(std::move(copy.error()));
    }
    auto enumerator = SegmentTermsEnum::create(std::move(copy.value()));
    if (!enumerator) {
        return std::unexpected(std::move(enumerator.error()));
    }
    return std::unique_ptr<TermsEnum>(std::move(enumerator.value()));
}

bool FieldReader::has_freqs() const { return field_info_->has_freqs(); }

bool FieldReader::has_offsets() const { return field_info_->has_offsets(); }

bool FieldReader::has_positions() const { return field_info_->has_positions(); }

bool FieldReader::has_payloads() const { return field_info_->has_payloads(); }

Result<std::optional<BytesRef>> FieldReader::get_min() const { return std::optional<BytesRef>(min_term_); }

Result<std::optional<BytesRef>> FieldReader::get_max() const { return std::optional<BytesRef>(max_term_); }

std::string FieldReader::to_string() const {
    return std::format("FieldReader(field={},numTerms={},hasFreqs={},hasPositions={},hasOffsets={},"
                       "hasPayloads={})",
                       field_info_->name, num_terms_, has_freqs(), has_positions(), has_offsets(), has_payloads());
}

Result<int64_t> FieldReader::read_vlong_output(DataInput &input) const {
    if (parent_ == nullptr) {
        return err(LuceneError::illegal_state("block-tree terms reader is missing"));
    }
    // Only the current format (MSB vlong outputs) is read, see the project's
    // "current version only" rule.
    debug_check(parent_->version() >= BLOCK_TREE_VERSION_MSB_VLONG_OUTPUT);
    return read_msb_vlong(input);
}

} // namespace lucene
