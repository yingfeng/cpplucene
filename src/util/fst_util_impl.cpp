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

module lucene_core:fst_util.impl;

import :fst_util;

import :bit_table_util;
import :bytes_ref;
import :bytes_ref_builder;
import :debug;
import :error;
import :fst;
import :fst_bytes_reader;
import :fst_outputs;
import :ints_ref;
import :types;

import std;

namespace lucene {

namespace {

/// Lexicographic comparison of the used prefix of two input builders.
int32_t compare_input_prefixes(const IntsRefBuilder &a, const IntsRefBuilder &b) {
    const auto &ai = a.ints();
    const auto &bi = b.ints();
    const size_t common = std::min(a.length(), b.length());
    for (size_t i = 0; i < common; ++i) {
        if (ai[i] != bi[i]) {
            return ai[i] < bi[i] ? -1 : 1;
        }
    }
    if (a.length() == b.length()) {
        return 0;
    }
    return a.length() < b.length() ? -1 : 1;
}

/// GraphViz cannot render arbitrary bytes, so non printable labels are hex.
std::string printable_label(int32_t label) {
    if (label >= 0x20 && label <= 0x7d && label != 0x22 && label != 0x5c) {
        return std::string(1, static_cast<char>(label));
    }
    std::ostringstream out;
    out << "0x" << std::hex << label;
    return out.str();
}

/// Emits one dot state line; empty optionals leave the corresponding attribute
/// out (the spacing matches Lucene's output).
void emit_dot_state(std::string &out, std::string_view name, std::string_view shape, std::string_view color,
                    std::string_view label) {
    out.append("  ");
    out.append(name);
    out.append(" [");
    if (!shape.empty()) {
        out.append("shape=");
        out.append(shape);
    }
    out.push_back(' ');
    if (!color.empty()) {
        out.append("color=");
        out.append(color);
    }
    out.append(" label=\"");
    out.append(label);
    out.append("\" ]\n");
}

} // namespace

// ---- FSTPath ----

FSTPath::FSTPath(FstOutput output, const Arc &arc, IntsRefBuilder input, float boost, std::string context,
                 int32_t payload)
    : arc(arc), output(std::move(output)), input(std::move(input)), boost(boost), context(std::move(context)),
      payload(payload) {}

FSTPath FSTPath::new_path(FstOutput new_output, IntsRefBuilder new_input) const {
    return FSTPath(std::move(new_output), arc, std::move(new_input), boost, context, payload);
}

std::string FSTPath::to_string() const {
    return "input=" + input.to_ints_ref().to_string() + " output=<any>" + " context=" + context +
           " boost=" + std::to_string(boost) + " payload=" + std::to_string(payload);
}

// ---- TopNSearcher ----

Result<TopNSearcher> TopNSearcher::by_tie_break(const FST &fst, size_t top_n, size_t max_queue_depth,
                                                OutputComparator comparator) {
    OutputComparator inner = comparator;
    PathComparator path_comparator = [inner](const FSTPath &a, const FSTPath &b) -> int32_t {
        const int32_t cmp = inner(a.output, b.output);
        if (cmp != 0) {
            return cmp;
        }
        return compare_input_prefixes(a.input, b.input);
    };
    return TopNSearcher(fst, top_n, max_queue_depth, std::move(comparator), std::move(path_comparator));
}

TopNSearcher::TopNSearcher(const FST &fst, size_t top_n, size_t max_queue_depth, OutputComparator comparator,
                           PathComparator path_comparator)
    : fst_(&fst), reader_(&fst.reader()), top_n_(top_n), max_queue_depth_(max_queue_depth),
      comparator_(std::move(comparator)), path_comparator_(std::move(path_comparator)),
      base_(std::make_unique<TopNSearcherBase>()), queue_(std::deque<FSTPath>()) {}

int32_t TopNSearcher::compare_inputs(const IntsRefBuilder &a, const IntsRefBuilder &b) const {
    return compare_input_prefixes(a, b);
}

VoidResult TopNSearcher::insert_queue(FSTPath path) {
    if (!queue_.has_value()) {
        return {};
    }
    auto &queue = queue_.value();
    for (size_t i = 0; i < queue.size(); ++i) {
        const int32_t cmp = path_comparator_(path, queue[i]);
        if (cmp < 0) {
            queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(i), std::move(path));
            return {};
        }
        if (cmp == 0) {
            return {};
        }
    }
    queue.push_back(std::move(path));
    return {};
}

VoidResult TopNSearcher::add_if_competitive(FSTPath &path) {
    debug_check(queue_.has_value(), "add_if_competitive needs an open queue");
    const auto &outputs = fst_->outputs();
    const FstOutput output = outputs.add(path.output, path.arc.output());

    if (!queue_->empty() && queue_->size() == max_queue_depth_ && max_queue_depth_ > 0) {
        const FSTPath &bottom = queue_->back();
        const int32_t cmp = path_comparator_(path, bottom);
        if (cmp > 0) {
            return {};
        }
        if (cmp == 0) {
            auto appended = path.input.append(path.arc.label());
            if (!appended.has_value()) {
                return appended;
            }
            const int32_t tie = compare_inputs(bottom.input, path.input);
            path.input.set_length(path.input.length() - 1);
            debug_check(tie != 0, "equal paths must differ in their input");
            if (tie < 0) {
                return {};
            }
        }
    }

    IntsRefBuilder new_input;
    auto copied = new_input.copy_ints_ref(path.input.to_ints_ref());
    if (!copied.has_value()) {
        return copied;
    }
    auto appended = new_input.append(path.arc.label());
    if (!appended.has_value()) {
        return appended;
    }

    FSTPath new_path = path.new_path(std::move(output), std::move(new_input));
    if (base_->accept_partial_path(new_path)) {
        if (auto inserted = insert_queue(std::move(new_path)); !inserted.has_value()) {
            return inserted;
        }
        if (queue_.has_value() && queue_->size() == max_queue_depth_ + 1) {
            queue_->pop_back();
        }
    }
    return {};
}

VoidResult TopNSearcher::add_start_paths(const Arc &node, FstOutput start_output, bool allow_empty_string,
                                         IntsRefBuilder input) {
    return add_start_paths_with_context(node, std::move(start_output), allow_empty_string, std::move(input), 0.0F, "",
                                        -1);
}

VoidResult TopNSearcher::add_start_paths_with_context(const Arc &node, FstOutput start_output,
                                                      bool allow_empty_string, IntsRefBuilder input, float boost,
                                                      std::string context, int32_t payload) {
    FSTPath path(std::move(start_output), node, std::move(input), boost, std::move(context), payload);
    auto first = fst_->read_first_target_arc(node, path.arc, *reader_);
    if (!first.has_value()) {
        return first;
    }
    while (true) {
        if (allow_empty_string || path.arc.label() != END_LABEL) {
            if (auto added = add_if_competitive(path); !added.has_value()) {
                return added;
            }
        }
        if (path.arc.is_last()) {
            return {};
        }
        if (auto next = fst_->read_next_arc(path.arc, *reader_); !next.has_value()) {
            return next;
        }
    }
}

Result<TopResults> TopNSearcher::search() {
    TopResults out;
    const auto &outputs = fst_->outputs();
    const FstOutput no_output = outputs.get_no_output();
    size_t reject_count = 0;

    while (out.top_n.size() < top_n_) {
        if (!queue_.has_value() || queue_->empty()) {
            break;
        }

        FSTPath path = std::move(queue_->front());
        queue_->pop_front();

        if (!base_->accept_partial_path(path)) {
            continue;
        }

        if (path.arc.label() == END_LABEL) {
            path.input.set_length(path.input.length() - 1);
            TopResult result;
            result.input = path.input.to_ints_ref();
            result.output = std::move(path.output);
            out.top_n.push_back(std::move(result));
            continue;
        }

        if (top_n_ > 0 && out.top_n.size() == top_n_ - 1 && max_queue_depth_ == top_n_) {
            // The last slot is about to be filled: no further competition.
            queue_.reset();
        }

        while (true) {
            const Arc follow = path.arc;
            if (auto first = fst_->read_first_target_arc(follow, path.arc, *reader_); !first.has_value()) {
                return err(first.error());
            }

            bool found_zero = false;
            bool arc_copy_is_pending = false;
            // `true` once the first zero-output arc was stashed in `scratch_arc_`.
            //
            // Lucene restores `path.arc` from its scratch slot unconditionally
            // when no copy is pending, which yields a default-constructed arc for
            // nodes that have no zero-output arc at all (possible with
            // byte-sequence outputs).  Tracking validity keeps the last read arc
            // instead, which is what the following step expects.
            bool scratch_valid = false;
            while (true) {
                // A slot that was never assigned an output is empty rather than a
                // materialised `no_output`; normalise it here because the
                // comparator belongs to the caller and may `any_cast` it.
                const FstOutput &arc_output =
                    path.arc.output().has_value() ? path.arc.output() : no_output;
                if (comparator_(no_output, arc_output) == 0) {
                    if (!queue_.has_value()) {
                        found_zero = true;
                        break;
                    }
                    if (!found_zero) {
                        arc_copy_is_pending = true;
                        found_zero = true;
                    } else {
                        if (auto added = add_if_competitive(path); !added.has_value()) {
                            return err(added.error());
                        }
                    }
                } else if (queue_.has_value()) {
                    if (auto added = add_if_competitive(path); !added.has_value()) {
                        return err(added.error());
                    }
                }
                if (path.arc.is_last()) {
                    break;
                }
                if (arc_copy_is_pending) {
                    scratch_arc_.copy_from(path.arc);
                    arc_copy_is_pending = false;
                    scratch_valid = true;
                }
                if (auto next = fst_->read_next_arc(path.arc, *reader_); !next.has_value()) {
                    return err(next.error());
                }
            }

            if (queue_.has_value() && !arc_copy_is_pending && scratch_valid) {
                path.arc.copy_from(scratch_arc_);
            }

            if (path.arc.label() == END_LABEL) {
                // `path.input` already holds the parent arc's label, so it must
                // not be shortened here (Lucene does the same).
                path.output = outputs.add(path.output, path.arc.output());
                if (base_->accept_result_path(path)) {
                    TopResult result;
                    result.input = path.input.to_ints_ref();
                    result.output = std::move(path.output);
                    out.top_n.push_back(std::move(result));
                } else {
                    ++reject_count;
                }
                break;
            }

            if (auto appended = path.input.append(path.arc.label()); !appended.has_value()) {
                return err(appended.error());
            }
            path.output = outputs.add(path.output, path.arc.output());
            if (!base_->accept_partial_path(path)) {
                break;
            }
        }
    }

    out.is_complete = reject_count + top_n_ <= max_queue_depth_;
    return out;
}

// ---- FstUtil ----

Result<std::optional<FstOutput>> FstUtil::get_from_ints(const FST &fst, std::span<const int32_t> input) {
    const auto &outputs = fst.outputs();
    const FstOutput no_output = outputs.get_no_output();
    Arc arc = Arc::with_outputs(no_output, no_output);
    fst.get_first_arc(arc);
    FstOutput output = no_output;
    FstBytesReader &reader = fst.reader();

    for (const int32_t label : input) {
        auto found = fst.find_target_arc(label, nullptr, arc, reader);
        if (!found.has_value()) {
            return err(found.error());
        }
        if (!found.value()) {
            return std::optional<FstOutput>{};
        }
        outputs.add_assign(output, arc.output_mut());
    }

    if (!arc.is_final()) {
        return std::optional<FstOutput>{};
    }
    outputs.add_assign(output, arc.next_final_output_mut());
    return std::optional<FstOutput>(std::move(output));
}

Result<std::optional<FstOutput>> FstUtil::get_from_bytes(const FST &fst, const BytesRef &input) {
    debug_check(fst.metadata().input_type == InputType::kByte1, "get_from_bytes needs a BYTE1 FST");
    const auto bytes = input.as_slice();
    std::vector<int32_t> labels;
    labels.reserve(bytes.size());
    for (const uint8_t byte : bytes) {
        labels.push_back(static_cast<int32_t>(byte));
    }
    return get_from_ints(fst, labels);
}

Result<bool> FstUtil::read_ceil_arc(int32_t label, const FST &fst, const Arc &follow, Arc &arc) {
    if (label == END_LABEL) {
        // `read_end_arc` reports whether `follow` is final, which is exactly
        // whether the END_LABEL arc exists.
        return read_end_arc(follow, arc);
    }

    if (!target_has_arcs(follow)) {
        return false;
    }

    FstBytesReader &reader = fst.reader();
    if (auto first = fst.read_first_target_arc(follow, arc, reader); !first.has_value()) {
        return err(first.error());
    }

    if (arc.bytes_per_arc() != 0 && arc.label() != END_LABEL) {
        switch (arc.node_flags()) {
            case ARCS_FOR_DIRECT_ADDRESSING: {
                const int32_t target_index = label - arc.label();
                if (target_index >= arc.num_arcs()) {
                    return false;
                }
                if (target_index < 0) {
                    return true;
                }
                reader.set_position(arc.bit_table_start());
                auto is_set = BitTableUtil::is_bit_set(target_index, reader);
                if (!is_set.has_value()) {
                    return err(is_set.error());
                }
                if (is_set.value()) {
                    reader.set_position(arc.bit_table_start());
                    if (auto read = fst.read_arc_by_direct_addressing(arc, reader, target_index); !read.has_value()) {
                        return err(read.error());
                    }
                    debug_check(arc.label() == label, "a set presence bit must select the wanted label");
                } else {
                    reader.set_position(arc.bit_table_start());
                    auto ceil_index = BitTableUtil::next_bit_set(target_index, get_num_presence_bytes(arc.num_arcs()),
                                                                reader);
                    if (!ceil_index.has_value()) {
                        return err(ceil_index.error());
                    }
                    debug_check(ceil_index.value() != -1, "a non-last arc must have a higher label");
                    if (auto read = fst.read_arc_by_direct_addressing(arc, reader, ceil_index.value());
                        !read.has_value()) {
                        return err(read.error());
                    }
                    debug_check(arc.label() > label, "the ceiling arc must carry a higher label");
                }
                return true;
            }

            case ARCS_FOR_CONTINUOUS: {
                const int32_t target_index = label - arc.label();
                if (target_index >= arc.num_arcs()) {
                    return false;
                }
                if (target_index < 0) {
                    return true;
                }
                if (auto read = fst.read_arc_by_continuous(arc, reader, target_index); !read.has_value()) {
                    return err(read.error());
                }
                debug_check(arc.label() == label, "continuous arcs are addressed by label");
                return true;
            }

            default: {
                // Fixed length arcs in a binary-search node.
                auto found = binary_search(fst, arc, label);
                if (!found.has_value()) {
                    return err(found.error());
                }
                const int32_t idx = found.value();
                if (idx >= 0) {
                    // Exact hit.
                    reader.set_position(arc.pos_arcs_start());
                    if (auto read = fst.read_arc_by_index(arc, reader, idx); !read.has_value()) {
                        return err(read.error());
                    }
                    return true;
                }
                // Miss: the insertion point is the arc with the smallest label
                // greater than the wanted one, which is the ceiling arc.
                const int32_t ceiling = -1 - idx;
                if (ceiling == arc.num_arcs()) {
                    // DEAD END: the wanted label is beyond the highest arc.
                    return false;
                }
                reader.set_position(arc.pos_arcs_start());
                if (auto read = fst.read_arc_by_index(arc, reader, ceiling); !read.has_value()) {
                    return err(read.error());
                }
                debug_check(arc.label() > label, "the ceiling arc must carry a higher label");
                return true;
            }
        }
    }

    if (auto first_real = fst.read_first_real_target_arc(follow.target(), arc, reader); !first_real.has_value()) {
        return err(first_real.error());
    }
    while (true) {
        if (arc.label() >= label) {
            return true;
        }
        if (arc.is_last()) {
            return false;
        }
        if (auto next = fst.read_next_real_arc(arc, reader); !next.has_value()) {
            return err(next.error());
        }
    }
}

Result<int32_t> FstUtil::binary_search(const FST &fst, const Arc &arc, int32_t target_label) {
    debug_check(arc.node_flags() == ARCS_FOR_BINARY_SEARCH, "binary search needs a packed arc array");
    FstBytesReader &reader = fst.reader();
    int32_t low = arc.arc_idx();
    int32_t high = arc.num_arcs() - 1;

    while (low <= high) {
        const int32_t mid = (low + high) >> 1;
        reader.set_position(arc.pos_arcs_start());
        if (auto skipped = reader.skip_bytes(static_cast<int64_t>(arc.bytes_per_arc()) * mid + 1);
            !skipped.has_value()) {
            return err(skipped.error());
        }
        auto mid_label = fst.read_label(reader);
        if (!mid_label.has_value()) {
            return err(mid_label.error());
        }
        const int32_t cmp = mid_label.value() - target_label;
        if (cmp < 0) {
            low = mid + 1;
        } else if (cmp > 0) {
            high = mid - 1;
        } else {
            return mid;
        }
    }

    return -1 - low;
}

Result<TopResults> FstUtil::shortest_paths(const FST &fst, const Arc &from_node, FstOutput start_output,
                                           OutputComparator comparator, size_t top_n, bool allow_empty_string) {
    auto searcher_result = TopNSearcher::by_tie_break(fst, top_n, top_n, std::move(comparator));
    if (!searcher_result.has_value()) {
        return err(searcher_result.error());
    }
    TopNSearcher searcher = std::move(searcher_result.value());
    if (auto added = searcher.add_start_paths(from_node, std::move(start_output), allow_empty_string, IntsRefBuilder{});
        !added.has_value()) {
        return err(added.error());
    }
    return searcher.search();
}

VoidResult FstUtil::to_dot(const FST &fst, std::string &out, bool same_rank, bool label_states) {
    constexpr std::string_view expanded_node_color = "blue";
    constexpr std::string_view state_shape = "circle";
    constexpr std::string_view final_state_shape = "doublecircle";

    Arc start_arc;
    fst.get_first_arc(start_arc);

    std::vector<Arc> this_level_queue;
    std::vector<Arc> next_level_queue;
    next_level_queue.push_back(start_arc);

    std::vector<int64_t> same_level_states;
    std::unordered_set<int64_t> seen;
    seen.insert(start_arc.target());

    out.append("digraph FST {\n");
    out.append("  rankdir = LR; splines=true; concentrate=true; ordering=out; ranksep=2.5; \n");

    if (!label_states) {
        out.append("  node [shape=circle, width=.2, height=.2, style=filled]\n");
    }

    emit_dot_state(out, "initial", "point", "white", "");

    const auto &outputs = fst.outputs();
    const FstOutput no_output = outputs.get_no_output();
    FstBytesReader &reader = fst.reader();

    {
        auto expanded = fst.is_expanded_target(start_arc, reader);
        if (!expanded.has_value()) {
            return err(expanded.error());
        }
        const std::string_view state_color = expanded.value() ? expanded_node_color : std::string_view{};

        bool is_final = false;
        std::string final_output;
        if (start_arc.is_final()) {
            is_final = true;
            if (!outputs.is_no_output(start_arc.next_final_output())) {
                final_output = outputs.output_to_string(start_arc.next_final_output());
            }
        }
        emit_dot_state(out, std::to_string(start_arc.target()), is_final ? final_state_shape : state_shape,
                       state_color, final_output);
    }

    out.append("  initial -> " + std::to_string(start_arc.target()) + "\n");

    int32_t level = 0;

    while (!next_level_queue.empty()) {
        this_level_queue.insert(this_level_queue.end(), next_level_queue.begin(), next_level_queue.end());
        next_level_queue.clear();

        ++level;
        out.append("\n  // Transitions and states at level: " + std::to_string(level) + "\n");
        while (!this_level_queue.empty()) {
            Arc arc = this_level_queue.back();
            this_level_queue.pop_back();
            if (!target_has_arcs(arc)) {
                continue;
            }

            const int64_t node = arc.target();
            if (auto first = fst.read_first_real_target_arc(arc.target(), arc, reader); !first.has_value()) {
                return err(first.error());
            }

            while (true) {
                if (arc.target() >= 0 && !seen.contains(arc.target())) {
                    auto expanded = fst.is_expanded_target(arc, reader);
                    if (!expanded.has_value()) {
                        return err(expanded.error());
                    }
                    const std::string_view state_color = expanded.value() ? expanded_node_color : std::string_view{};

                    std::string final_output;
                    if (!outputs.is_no_output(arc.next_final_output())) {
                        final_output = outputs.output_to_string(arc.next_final_output());
                    }

                    emit_dot_state(out, std::to_string(arc.target()), state_shape, state_color, final_output);
                    seen.insert(arc.target());
                    next_level_queue.push_back(arc);
                    same_level_states.push_back(arc.target());
                }

                std::string outs;
                if (!outputs.is_no_output(arc.output())) {
                    outs = "/" + outputs.output_to_string(arc.output());
                }
                if (!target_has_arcs(arc) && arc.is_final() && !outputs.is_no_output(arc.next_final_output())) {
                    outs += "/[" + outputs.output_to_string(arc.next_final_output()) + "]";
                }

                const std::string_view arc_color = has_flag(arc.flags(), BIT_TARGET_NEXT) ? "red" : "black";

                debug_check(arc.label() != END_LABEL, "real arcs never carry END_LABEL");

                out.append("  " + std::to_string(node) + " -> " + std::to_string(arc.target()) + " [label=\"" +
                           printable_label(arc.label()) + outs + "\"" + (arc.is_final() ? " style=\"bold\"" : "") +
                           " color=\"" + std::string(arc_color) + "\"]\n");

                if (arc.is_last()) {
                    break;
                }
                if (auto next = fst.read_next_real_arc(arc, reader); !next.has_value()) {
                    return err(next.error());
                }
            }
        }

        if (same_rank && same_level_states.size() > 1) {
            out.append("  {rank=same; ");
            for (const int64_t state : same_level_states) {
                out.append(std::to_string(state) + "; ");
            }
            out.append(" }\n");
        }
        same_level_states.clear();
    }

    out.append("  -1 [style=filled, color=black, shape=doublecircle, label=\"\"]\n\n");
    out.append("  {rank=sink; -1 }\n");
    out.append("}\n");
    return {};
}

VoidResult FstUtil::to_utf32(std::string_view text, IntsRefBuilder &scratch) {
    size_t index = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<uint8_t>(text[offset]);
        uint32_t code_point = 0;
        size_t continuation = 0;
        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1FU;
            continuation = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0FU;
            continuation = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07U;
            continuation = 3;
        } else {
            return err(LuceneError::illegal_argument("invalid UTF-8 lead byte in FST input"));
        }
        if (offset + continuation >= text.size()) {
            return err(LuceneError::illegal_argument("truncated UTF-8 sequence in FST input"));
        }
        for (size_t i = 1; i <= continuation; ++i) {
            const auto next = static_cast<uint8_t>(text[offset + i]);
            if ((next & 0xC0) != 0x80) {
                return err(LuceneError::illegal_argument("invalid UTF-8 continuation byte in FST input"));
            }
            code_point = (code_point << 6) | (next & 0x3FU);
        }
        if (auto grown = scratch.grow(index + 1); !grown.has_value()) {
            return grown;
        }
        scratch.set_int_at(index, static_cast<int32_t>(code_point));
        ++index;
        offset += continuation + 1;
    }
    scratch.set_length(index);
    return {};
}

VoidResult FstUtil::to_ints_ref(const BytesRef &input, IntsRefBuilder &scratch) {
    const auto bytes = input.as_slice();
    if (auto grown = scratch.grow_no_copy(bytes.size()); !grown.has_value()) {
        return grown;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        scratch.set_int_at(i, static_cast<int32_t>(bytes[i]));
    }
    scratch.set_length(bytes.size());
    return {};
}

Result<BytesRef> FstUtil::to_bytes_ref(const IntsRef &input, BytesRefBuilder &scratch) {
    const auto labels = input.as_slice();
    if (auto grown = scratch.grow(labels.size()); !grown.has_value()) {
        return err(grown.error());
    }
    for (size_t i = 0; i < labels.size(); ++i) {
        debug_check(labels[i] >= 0 && labels[i] <= 0xFF, "an FST label must fit into one byte");
        scratch.set_byte_at(i, static_cast<uint8_t>(labels[i]));
    }
    scratch.set_length(labels.size());
    return scratch.get_bytes_ref();
}

} // namespace lucene
