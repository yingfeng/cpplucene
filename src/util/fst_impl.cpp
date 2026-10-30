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

module lucene_core:fst.impl;

import :fst;
import :bit_table_util;
import :codec_util;
import :fst_bytes_reader;
import :fst_outputs;
import :growable_byte_array_data_output;
import :reverse_bytes_reader;
import :types;
import :error;
import :debug;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

/// The `BitTableUtil` helpers all expect the reader to sit at the presence bits.
FstBytesReader &positioned_at_bit_table(const Arc &arc, FstBytesReader &reader) {
    reader.set_position(arc.bit_table_start());
    return reader;
}

} // namespace

Result<int32_t> read_label(DataInput &input, InputType input_type, int32_t version) {
    switch (input_type) {
        case InputType::kByte1: {
            auto byte = input.read_byte();
            if (!byte.has_value()) {
                return err(byte.error());
            }
            return static_cast<int32_t>(byte.value());
        }
        case InputType::kByte2: {
            auto value = input.read_short();
            if (!value.has_value()) {
                return err(value.error());
            }
            if (version < VERSION_LITTLE_ENDIAN) {
                // Big-endian files: swap the two bytes back.
                const auto raw = static_cast<uint16_t>(value.value());
                return static_cast<int32_t>((raw >> 8) | ((raw & 0xFF) << 8));
            }
            return static_cast<int32_t>(static_cast<uint16_t>(value.value()));
        }
        case InputType::kByte4:
            return input.read_vint();
    }
    return err(LuceneError::corrupt_index("invalid input type"));
}

VoidResult write_label(DataOutput &out, InputType input_type, int32_t label) {
    switch (input_type) {
        case InputType::kByte1:
            return out.write_byte(static_cast<uint8_t>(label));
        case InputType::kByte2:
            return out.write_short(static_cast<int16_t>(label));
        case InputType::kByte4:
            return out.write_vint(label);
    }
    return err(LuceneError::illegal_state("invalid input type"));
}

Arc Arc::with_outputs(FstOutput output, FstOutput next_final_output) {
    Arc arc;
    arc.output_ = std::move(output);
    arc.next_final_output_ = std::move(next_final_output);
    return arc;
}

void Arc::copy_from(const Arc &other) {
    label_ = other.label_;
    output_ = other.output_;
    target_ = other.target_;
    flags_ = other.flags_;
    next_final_output_ = other.next_final_output_;
    next_arc_ = other.next_arc_;
    node_flags_ = other.node_flags_;
    bytes_per_arc_ = other.bytes_per_arc_;
    pos_arcs_start_ = other.pos_arcs_start_;
    arc_idx_ = other.arc_idx_;
    num_arcs_ = other.num_arcs_;
    bit_table_start_ = other.bit_table_start_;
    first_label_ = other.first_label_;
    presence_index_ = other.presence_index_;
}

std::string Arc::to_string() const {
    return "Arc(label=" + std::to_string(label_) + ", target=" + std::to_string(target_) +
           ", flags=" + std::to_string(flags_) + ", nodeFlags=" + std::to_string(node_flags_) +
           ", bytesPerArc=" + std::to_string(bytes_per_arc_) + ")";
}

bool read_end_arc(const Arc &follow, Arc &arc) {
    if (!follow.is_final()) {
        return false;
    }
    if (follow.target() <= 0) {
        arc.set_flags(BIT_LAST_ARC);
    } else {
        arc.set_flags(0);
        // NOTE: `next_arc` is a node id, not an address, in this case.
        arc.set_next_arc(follow.target());
    }
    arc.set_output(follow.next_final_output());
    arc.set_label(END_LABEL);
    return true;
}

FST::FST(FSTMetadata metadata, std::unique_ptr<FstBytesReader> reader)
    : metadata_(std::move(metadata)), reader_(std::move(reader)) {}

Result<int32_t> FST::read_label(DataInput &input) const {
    return lucene::read_label(input, metadata_.input_type, metadata_.version);
}

void FST::get_first_arc(Arc &arc) const {
    const auto &outputs = *metadata_.outputs;
    if (!metadata_.empty_output.has_value()) {
        arc.set_flags(BIT_LAST_ARC);
        arc.set_output(outputs.get_no_output());
        arc.set_next_final_output(outputs.get_no_output());
    } else {
        uint8_t flags = BIT_FINAL_ARC | BIT_LAST_ARC;
        if (!outputs.is_no_output(metadata_.empty_output.value())) {
            flags |= BIT_ARC_HAS_FINAL_OUTPUT;
        }
        arc.set_flags(flags);
        arc.set_output(outputs.get_no_output());
        arc.set_next_final_output(metadata_.empty_output.value());
    }
    // When the FST only accepts the empty string, `start_node` is 0.
    arc.set_target(metadata_.start_node);
}

VoidResult FST::read_presence_bytes(Arc &arc, FstBytesReader &reader) const {
    debug_check(arc.bytes_per_arc() > 0, "a direct-addressing node has fixed length arcs");
    debug_check(arc.node_flags() == ARCS_FOR_DIRECT_ADDRESSING, "only direct-addressing nodes have presence bits");
    arc.set_bit_table_start(reader.get_position());
    return reader.skip_bytes(get_num_presence_bytes(arc.num_arcs()));
}

Result<int64_t> FST::read_unpacked_node_target(FstBytesReader &reader) const {
    return reader.read_vlong();
}

VoidResult FST::read_first_arc_info(int64_t node_address, Arc &arc, FstBytesReader &reader) const {
    reader.set_position(node_address);
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    arc.set_node_flags(flags.value());

    if (flags.value() == ARCS_FOR_BINARY_SEARCH || flags.value() == ARCS_FOR_DIRECT_ADDRESSING ||
        flags.value() == ARCS_FOR_CONTINUOUS) {
        auto num_arcs = reader.read_vint();
        if (!num_arcs.has_value()) {
            return err(num_arcs.error());
        }
        auto bytes_per_arc = reader.read_vint();
        if (!bytes_per_arc.has_value()) {
            return err(bytes_per_arc.error());
        }
        arc.set_num_arcs(num_arcs.value());
        arc.set_bytes_per_arc(bytes_per_arc.value());
        arc.set_arc_idx(-1);

        if (flags.value() == ARCS_FOR_DIRECT_ADDRESSING) {
            if (auto result = read_presence_bytes(arc, reader); !result.has_value()) {
                return result;
            }
            auto first_label = read_label(reader);
            if (!first_label.has_value()) {
                return err(first_label.error());
            }
            arc.set_first_label(first_label.value());
            arc.set_presence_index(-1);
        } else if (flags.value() == ARCS_FOR_CONTINUOUS) {
            auto first_label = read_label(reader);
            if (!first_label.has_value()) {
                return err(first_label.error());
            }
            arc.set_first_label(first_label.value());
        }
        arc.set_pos_arcs_start(reader.get_position());
    } else {
        arc.set_next_arc(node_address);
        arc.set_bytes_per_arc(0);
    }
    return {};
}

VoidResult FST::read_first_real_target_arc(int64_t node_address, Arc &arc, FstBytesReader &reader) const {
    if (auto result = read_first_arc_info(node_address, arc, reader); !result.has_value()) {
        return result;
    }
    return read_next_real_arc(arc, reader);
}

VoidResult FST::read_first_target_arc(const Arc &follow, Arc &arc, FstBytesReader &reader) const {
    if (follow.is_final()) {
        // Insert the fake final arc to END_LABEL.
        arc.set_label(END_LABEL);
        arc.set_output(follow.next_final_output());
        arc.set_flags(BIT_FINAL_ARC);
        if (follow.target() <= 0) {
            arc.set_flags(static_cast<uint8_t>(arc.flags() | BIT_LAST_ARC));
        } else {
            arc.set_next_arc(follow.target());
        }
        arc.set_target(FINAL_END_NODE);
        arc.set_node_flags(arc.flags());
        return {};
    }
    return read_first_real_target_arc(follow.target(), arc, reader);
}

VoidResult FST::read_last_target_arc(const Arc &follow, Arc &arc, FstBytesReader &reader) const {
    if (!target_has_arcs(follow)) {
        debug_check(follow.is_final(), "a node without arcs must be final");
        arc.set_label(END_LABEL);
        arc.set_target(FINAL_END_NODE);
        arc.set_output(follow.next_final_output());
        arc.set_flags(BIT_LAST_ARC);
        arc.set_node_flags(BIT_LAST_ARC);
        return {};
    }

    reader.set_position(follow.target());
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    arc.set_node_flags(flags.value());

    if (flags.value() == ARCS_FOR_BINARY_SEARCH || flags.value() == ARCS_FOR_DIRECT_ADDRESSING ||
        flags.value() == ARCS_FOR_CONTINUOUS) {
        // Jump straight to the last arc of a fixed-length node.
        auto num_arcs = reader.read_vint();
        if (!num_arcs.has_value()) {
            return err(num_arcs.error());
        }
        auto bytes_per_arc = reader.read_vint();
        if (!bytes_per_arc.has_value()) {
            return err(bytes_per_arc.error());
        }
        arc.set_num_arcs(num_arcs.value());
        arc.set_bytes_per_arc(bytes_per_arc.value());
        arc.set_arc_idx(arc.num_arcs() - 1);

        if (flags.value() == ARCS_FOR_DIRECT_ADDRESSING) {
            if (auto result = read_presence_bytes(arc, reader); !result.has_value()) {
                return result;
            }
            auto first_label = read_label(reader);
            if (!first_label.has_value()) {
                return err(first_label.error());
            }
            arc.set_first_label(first_label.value());
            return read_last_arc_by_direct_addressing(arc, reader);
        }
        if (flags.value() == ARCS_FOR_CONTINUOUS) {
            auto first_label = read_label(reader);
            if (!first_label.has_value()) {
                return err(first_label.error());
            }
            arc.set_first_label(first_label.value());
        }
        arc.set_pos_arcs_start(reader.get_position());
        return read_last_arc_by_continuous(arc, reader);
    }

    // Variable-length arcs: scan to the last one.
    if (auto result = read_next_real_arc(arc, reader); !result.has_value()) {
        return result;
    }
    while (!arc.is_last()) {
        if (auto result = read_next_real_arc(arc, reader); !result.has_value()) {
            return result;
        }
    }
    return {};
}

VoidResult FST::read_next_arc(Arc &arc, FstBytesReader &reader) const {
    if (arc.label() == END_LABEL) {
        // The fake final arc inserted for a final follow arc.
        if (arc.next_arc() <= 0) {
            return err(LuceneError::illegal_argument("cannot read_next_arc when arc.is_last()=true"));
        }
        return read_first_real_target_arc(arc.next_arc(), arc, reader);
    }
    return read_next_real_arc(arc, reader);
}

VoidResult FST::read_next_real_arc(Arc &arc, FstBytesReader &reader) const {
    switch (arc.node_flags()) {
        case ARCS_FOR_BINARY_SEARCH:
        case ARCS_FOR_CONTINUOUS: {
            debug_check(arc.bytes_per_arc() > 0, "fixed length arcs");
            arc.set_arc_idx(arc.arc_idx() + 1);
            debug_check(arc.arc_idx() >= 0 && arc.arc_idx() < arc.num_arcs(), "arc index out of range");
            reader.set_position(arc.pos_arcs_start() -
                                static_cast<int64_t>(arc.arc_idx()) * arc.bytes_per_arc());
            auto flags = reader.read_byte();
            if (!flags.has_value()) {
                return err(flags.error());
            }
            arc.set_flags(flags.value());
            break;
        }
        case ARCS_FOR_DIRECT_ADDRESSING: {
            // A freshly entered node has `arc_idx == -1`: start at bit 0.
            const int32_t from = arc.arc_idx() < 0 ? 0 : arc.arc_idx();
            auto next_index =
                BitTableUtil::next_bit_set(from, get_num_presence_bytes(arc.num_arcs()),
                                           positioned_at_bit_table(arc, reader));
            if (!next_index.has_value()) {
                return err(next_index.error());
            }
            return read_arc_by_direct_addressing_with_presence_index(arc, reader, next_index.value(),
                                                                     arc.presence_index() + 1);
        }
        default: {
            // Variable-length arcs: linear scan.
            debug_check(arc.bytes_per_arc() == 0, "variable length nodes have no fixed arc size");
            reader.set_position(arc.next_arc());
            auto flags = reader.read_byte();
            if (!flags.has_value()) {
                return err(flags.error());
            }
            arc.set_flags(flags.value());
            break;
        }
    }
    return read_arc(arc, reader);
}

VoidResult FST::read_arc(Arc &arc, FstBytesReader &reader) const {
    if (arc.node_flags() == ARCS_FOR_DIRECT_ADDRESSING || arc.node_flags() == ARCS_FOR_CONTINUOUS) {
        arc.set_label(arc.first_label() + arc.arc_idx());
    } else {
        auto label = read_label(reader);
        if (!label.has_value()) {
            return err(label.error());
        }
        arc.set_label(label.value());
    }

    const auto &outputs = *metadata_.outputs;
    if (has_flag(arc.flags(), BIT_ARC_HAS_OUTPUT)) {
        auto output = outputs.read(reader);
        if (!output.has_value()) {
            return err(output.error());
        }
        arc.set_output(output.value());
    } else {
        // An empty `FstOutput` is NO_OUTPUT for every outputs implementation (they
        // answer their empty value for a slot that does not hold their type), and
        // unlike `get_no_output()` it does not allocate: a `BytesRef` does not fit
        // the small-object buffer.  Reading an arc is on the seek path of every
        // term, so this used to allocate twice per arc.
        arc.set_output(FstOutput{});
    }

    if (has_flag(arc.flags(), BIT_ARC_HAS_FINAL_OUTPUT)) {
        auto output = outputs.read_final_output(reader);
        if (!output.has_value()) {
            return err(output.error());
        }
        arc.set_next_final_output(output.value());
    } else {
        arc.set_next_final_output(FstOutput{});
    }

    if (has_flag(arc.flags(), BIT_STOP_NODE)) {
        arc.set_target(has_flag(arc.flags(), BIT_FINAL_ARC) ? FINAL_END_NODE : NON_FINAL_END_NODE);
        arc.set_next_arc(reader.get_position());
    } else if (has_flag(arc.flags(), BIT_TARGET_NEXT)) {
        arc.set_next_arc(reader.get_position());
        if (!has_flag(arc.flags(), BIT_LAST_ARC)) {
            if (arc.bytes_per_arc() == 0) {
                if (auto result = seek_to_next_node(reader); !result.has_value()) {
                    return result;
                }
            } else {
                int32_t num_arcs = arc.num_arcs();
                if (arc.node_flags() == ARCS_FOR_DIRECT_ADDRESSING) {
                    // A direct-addressing node stores one arc per *present* bit.
                    auto count = BitTableUtil::count_bits(get_num_presence_bytes(arc.num_arcs()),
                                                          positioned_at_bit_table(arc, reader));
                    if (!count.has_value()) {
                        return err(count.error());
                    }
                    num_arcs = count.value();
                }
                reader.set_position(arc.pos_arcs_start() - static_cast<int64_t>(arc.bytes_per_arc()) * num_arcs);
            }
        }
        arc.set_target(reader.get_position());
    } else {
        auto target = read_unpacked_node_target(reader);
        if (!target.has_value()) {
            return err(target.error());
        }
        arc.set_target(target.value());
        arc.set_next_arc(reader.get_position());
    }
    return {};
}

VoidResult FST::read_arc_by_index(Arc &arc, FstBytesReader &reader, int32_t idx) const {
    debug_check(arc.bytes_per_arc() > 0, "binary search nodes have fixed length arcs");
    debug_check(arc.node_flags() == ARCS_FOR_BINARY_SEARCH, "only binary search nodes are indexable");
    debug_check(idx >= 0 && idx < arc.num_arcs(), "arc index out of range");
    reader.set_position(arc.pos_arcs_start() - static_cast<int64_t>(idx) * arc.bytes_per_arc());
    arc.set_arc_idx(idx);
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    arc.set_flags(flags.value());
    return read_arc(arc, reader);
}

VoidResult FST::read_arc_by_continuous(Arc &arc, FstBytesReader &reader, int32_t range_index) const {
    debug_check(range_index >= 0 && range_index < arc.num_arcs(), "range index out of range");
    reader.set_position(arc.pos_arcs_start() - static_cast<int64_t>(range_index) * arc.bytes_per_arc());
    arc.set_arc_idx(range_index);
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    arc.set_flags(flags.value());
    return read_arc(arc, reader);
}

VoidResult FST::read_arc_by_direct_addressing(Arc &arc, FstBytesReader &reader, int32_t range_index) const {
    debug_check(range_index >= 0 && range_index < arc.num_arcs(), "range index out of range");
    auto presence_index = BitTableUtil::count_bits_upto(range_index, positioned_at_bit_table(arc, reader));
    if (!presence_index.has_value()) {
        return err(presence_index.error());
    }
    return read_arc_by_direct_addressing_with_presence_index(arc, reader, range_index, presence_index.value());
}

VoidResult FST::read_arc_by_direct_addressing_with_presence_index(Arc &arc, FstBytesReader &reader,
                                                                 int32_t range_index, int32_t presence_index) const {
    reader.set_position(arc.pos_arcs_start() - static_cast<int64_t>(presence_index) * arc.bytes_per_arc());
    arc.set_arc_idx(range_index);
    arc.set_presence_index(presence_index);
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    arc.set_flags(flags.value());
    return read_arc(arc, reader);
}

VoidResult FST::read_last_arc_by_direct_addressing(Arc &arc, FstBytesReader &reader) const {
    auto presence_index = BitTableUtil::count_bits(get_num_presence_bytes(arc.num_arcs()),
                                                   positioned_at_bit_table(arc, reader));
    if (!presence_index.has_value()) {
        return err(presence_index.error());
    }
    return read_arc_by_direct_addressing_with_presence_index(arc, reader, arc.num_arcs() - 1,
                                                             presence_index.value() - 1);
}

VoidResult FST::read_last_arc_by_continuous(Arc &arc, FstBytesReader &reader) const {
    return read_arc_by_continuous(arc, reader, arc.num_arcs() - 1);
}

Result<bool> FST::is_expanded_target(const Arc &follow, FstBytesReader &reader) const {
    if (!target_has_arcs(follow)) {
        return false;
    }
    reader.set_position(follow.target());
    auto flags = reader.read_byte();
    if (!flags.has_value()) {
        return err(flags.error());
    }
    return flags.value() == ARCS_FOR_BINARY_SEARCH || flags.value() == ARCS_FOR_DIRECT_ADDRESSING ||
           flags.value() == ARCS_FOR_CONTINUOUS;
}

Result<bool> FST::find_target_arc(int32_t label_to_match, const Arc *follow, Arc &arc,
                                 FstBytesReader &reader) const {
    const Arc &from = follow != nullptr ? *follow : arc;
    const int64_t follow_target = follow != nullptr ? follow->target() : arc.target();

    if (label_to_match == END_LABEL) {
        if (!from.is_final()) {
            return false;
        }
        if (follow_target <= 0) {
            arc.set_flags(BIT_LAST_ARC);
        } else {
            arc.set_flags(0);
            arc.set_next_arc(follow_target);
        }
        arc.set_output(from.next_final_output());
        arc.set_label(END_LABEL);
        arc.set_node_flags(arc.flags());
        return true;
    }

    if (follow_target <= 0) {
        return false;
    }

    reader.set_position(follow_target);
    auto header = reader.read_byte();
    if (!header.has_value()) {
        return err(header.error());
    }
    arc.set_node_flags(header.value());

    if (header.value() == ARCS_FOR_DIRECT_ADDRESSING) {
        auto num_arcs = reader.read_vint();
        if (!num_arcs.has_value()) {
            return err(num_arcs.error());
        }
        auto bytes_per_arc = reader.read_vint();
        if (!bytes_per_arc.has_value()) {
            return err(bytes_per_arc.error());
        }
        arc.set_num_arcs(num_arcs.value());
        arc.set_bytes_per_arc(bytes_per_arc.value());
        if (auto result = read_presence_bytes(arc, reader); !result.has_value()) {
            return err(result.error());
        }
        auto first_label = read_label(reader);
        if (!first_label.has_value()) {
            return err(first_label.error());
        }
        arc.set_first_label(first_label.value());
        arc.set_pos_arcs_start(reader.get_position());

        const int32_t arc_index = label_to_match - arc.first_label();
        if (arc_index < 0 || arc_index >= arc.num_arcs()) {
            return false; // Before or after the label range.
        }
        auto present = BitTableUtil::is_bit_set(arc_index, positioned_at_bit_table(arc, reader));
        if (!present.has_value()) {
            return err(present.error());
        }
        if (!present.value()) {
            return false; // Arc missing inside the range.
        }
        if (auto result = read_arc_by_direct_addressing(arc, reader, arc_index); !result.has_value()) {
            return err(result.error());
        }
        return true;
    }

    if (header.value() == ARCS_FOR_BINARY_SEARCH) {
        auto num_arcs = reader.read_vint();
        if (!num_arcs.has_value()) {
            return err(num_arcs.error());
        }
        auto bytes_per_arc = reader.read_vint();
        if (!bytes_per_arc.has_value()) {
            return err(bytes_per_arc.error());
        }
        arc.set_num_arcs(num_arcs.value());
        arc.set_bytes_per_arc(bytes_per_arc.value());
        arc.set_pos_arcs_start(reader.get_position());

        // The array is sparse, so binary search it.
        int32_t low = 0;
        int32_t high = arc.num_arcs() - 1;
        while (low <= high) {
            const int32_t mid = (low + high) >> 1;
            reader.set_position(arc.pos_arcs_start() -
                                (static_cast<int64_t>(arc.bytes_per_arc()) * mid + 1));
            auto mid_label = read_label(reader);
            if (!mid_label.has_value()) {
                return err(mid_label.error());
            }
            if (mid_label.value() < label_to_match) {
                low = mid + 1;
            } else if (mid_label.value() > label_to_match) {
                high = mid - 1;
            } else {
                arc.set_arc_idx(mid - 1);
                if (auto result = read_next_real_arc(arc, reader); !result.has_value()) {
                    return err(result.error());
                }
                return true;
            }
        }
        return false;
    }

    if (header.value() == ARCS_FOR_CONTINUOUS) {
        auto num_arcs = reader.read_vint();
        if (!num_arcs.has_value()) {
            return err(num_arcs.error());
        }
        auto bytes_per_arc = reader.read_vint();
        if (!bytes_per_arc.has_value()) {
            return err(bytes_per_arc.error());
        }
        arc.set_num_arcs(num_arcs.value());
        arc.set_bytes_per_arc(bytes_per_arc.value());
        auto first_label = read_label(reader);
        if (!first_label.has_value()) {
            return err(first_label.error());
        }
        arc.set_first_label(first_label.value());
        arc.set_pos_arcs_start(reader.get_position());

        const int32_t arc_index = label_to_match - arc.first_label();
        if (arc_index < 0 || arc_index >= arc.num_arcs()) {
            return false; // Before or after the label range.
        }
        arc.set_arc_idx(arc_index - 1);
        if (auto result = read_next_real_arc(arc, reader); !result.has_value()) {
            return err(result.error());
        }
        return true;
    }

    // Variable-length arcs: linear scan.
    if (auto result = read_first_arc_info(follow_target, arc, reader); !result.has_value()) {
        return err(result.error());
    }
    reader.set_position(arc.next_arc());
    while (true) {
        debug_check(arc.bytes_per_arc() == 0, "a linear scan only happens on variable length nodes");
        auto flags = reader.read_byte();
        if (!flags.has_value()) {
            return err(flags.error());
        }
        arc.set_flags(flags.value());
        const int64_t label_position = reader.get_position();
        auto label = read_label(reader);
        if (!label.has_value()) {
            return err(label.error());
        }
        if (label.value() == label_to_match) {
            reader.set_position(label_position);
            if (auto result = read_arc(arc, reader); !result.has_value()) {
                return err(result.error());
            }
            return true;
        }
        if (label.value() > label_to_match || arc.is_last()) {
            return false;
        }
        const auto &outputs = *metadata_.outputs;
        if (has_flag(arc.flags(), BIT_ARC_HAS_OUTPUT)) {
            if (auto result = outputs.skip_output(reader); !result.has_value()) {
                return err(result.error());
            }
        }
        if (has_flag(arc.flags(), BIT_ARC_HAS_FINAL_OUTPUT)) {
            if (auto result = outputs.skip_final_output(reader); !result.has_value()) {
                return err(result.error());
            }
        }
        if (!has_flag(arc.flags(), BIT_STOP_NODE) && !has_flag(arc.flags(), BIT_TARGET_NEXT)) {
            if (auto target = read_unpacked_node_target(reader); !target.has_value()) {
                return err(target.error());
            }
        }
    }
}

VoidResult FST::seek_to_next_node(FstBytesReader &reader) const {
    while (true) {
        auto flags = reader.read_byte();
        if (!flags.has_value()) {
            return err(flags.error());
        }
        auto label = read_label(reader);
        if (!label.has_value()) {
            return err(label.error());
        }
        const auto &outputs = *metadata_.outputs;
        if (has_flag(flags.value(), BIT_ARC_HAS_OUTPUT)) {
            if (auto result = outputs.skip_output(reader); !result.has_value()) {
                return result;
            }
        }
        if (has_flag(flags.value(), BIT_ARC_HAS_FINAL_OUTPUT)) {
            if (auto result = outputs.skip_final_output(reader); !result.has_value()) {
                return result;
            }
        }
        if (!has_flag(flags.value(), BIT_STOP_NODE) && !has_flag(flags.value(), BIT_TARGET_NEXT)) {
            if (auto target = read_unpacked_node_target(reader); !target.has_value()) {
                return err(target.error());
            }
        }
        if (has_flag(flags.value(), BIT_LAST_ARC)) {
            return {};
        }
    }
}

VoidResult FSTMetadata::save(DataOutput &meta_out) const {
    if (outputs == nullptr) {
        return err(LuceneError::illegal_state("FSTMetadata needs an outputs instance"));
    }
    if (auto result = CodecUtil::write_header(meta_out, FILE_FORMAT_NAME, VERSION_CURRENT); !result.has_value()) {
        return result;
    }

    if (empty_output.has_value()) {
        if (auto result = meta_out.write_byte(1); !result.has_value()) {
            return result;
        }
        GrowableByteArrayDataOutput buffer;
        if (auto result = outputs->write_final_output(empty_output.value(), buffer); !result.has_value()) {
            return result;
        }
        auto bytes = buffer.take_bytes();
        // The reader walks the serialized output backwards.
        std::reverse(bytes.begin(), bytes.end());
        if (auto result = meta_out.write_vint(static_cast<int32_t>(bytes.size())); !result.has_value()) {
            return result;
        }
        if (auto result = meta_out.write_bytes(bytes); !result.has_value()) {
            return result;
        }
    } else if (auto result = meta_out.write_byte(0); !result.has_value()) {
        return result;
    }

    if (auto result = meta_out.write_byte(static_cast<uint8_t>(input_type)); !result.has_value()) {
        return result;
    }
    if (auto result = meta_out.write_vlong(start_node); !result.has_value()) {
        return result;
    }
    return meta_out.write_vlong(num_bytes);
}

Result<FSTMetadata> FSTMetadata::read(DataInput &meta_in, const FstOutputs &outputs) {
    auto version = CodecUtil::check_header(meta_in, FILE_FORMAT_NAME, VERSION_START, VERSION_CURRENT);
    if (!version.has_value()) {
        return err(version.error());
    }

    FSTMetadata metadata;
    metadata.outputs = &outputs;
    metadata.version = version.value();

    auto accepts_empty = meta_in.read_byte();
    if (!accepts_empty.has_value()) {
        return err(accepts_empty.error());
    }
    if (accepts_empty.value() == 1) {
        auto length = meta_in.read_vint();
        if (!length.has_value()) {
            return err(length.error());
        }
        if (length.value() < 0) {
            return err(LuceneError::corrupt_index("negative empty output length"));
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(length.value()));
        if (auto result = meta_in.read_bytes(bytes); !result.has_value()) {
            return err(result.error());
        }
        // The bytes are stored in reverse order (the FST reader only walks
        // backwards), so they must *not* be flipped again here: the reader is
        // positioned at the last byte instead.
        ReverseBytesReader reader(bytes);
        if (!bytes.empty()) {
            reader.set_position(static_cast<int64_t>(bytes.size()) - 1);
        }
        auto output = outputs.read_final_output(reader);
        if (!output.has_value()) {
            return err(output.error());
        }
        metadata.empty_output = output.value();
    }

    auto input_type = meta_in.read_byte();
    if (!input_type.has_value()) {
        return err(input_type.error());
    }
    switch (input_type.value()) {
        case 0:
            metadata.input_type = InputType::kByte1;
            break;
        case 1:
            metadata.input_type = InputType::kByte2;
            break;
        case 2:
            metadata.input_type = InputType::kByte4;
            break;
        default:
            return err(LuceneError::corrupt_index("invalid input type: " + std::to_string(input_type.value())));
    }

    auto start_node = meta_in.read_vlong();
    if (!start_node.has_value()) {
        return err(start_node.error());
    }
    auto num_bytes = meta_in.read_vlong();
    if (!num_bytes.has_value()) {
        return err(num_bytes.error());
    }
    metadata.start_node = start_node.value();
    metadata.num_bytes = num_bytes.value();
    return metadata;
}

} // namespace lucene
