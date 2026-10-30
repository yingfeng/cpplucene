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

module lucene_core:fst_builder.impl;

import :fst_builder;
import :fst;
import :fst_outputs;
import :fst_bytes_reader;
import :growable_byte_array_data_output;
import :types;
import :error;
import :debug;
import :data_output;

import std;

namespace lucene {

namespace {

/// Collects the bytes of one node before they are appended to the FST data.
class NodeBytes {
public:
    explicit NodeBytes(InputType input_type) : input_type_(input_type) {}

    void write_flags(uint8_t flags) { (void)buffer_.write_byte(flags); }

    [[nodiscard]] VoidResult write_label(int32_t label) { return lucene::write_label(buffer_, input_type_, label); }

    [[nodiscard]] VoidResult write_target(int64_t target) { return buffer_.write_vlong(target); }

    [[nodiscard]] GrowableByteArrayDataOutput &buffer() noexcept { return buffer_; }
    [[nodiscard]] const GrowableByteArrayDataOutput &buffer() const noexcept { return buffer_; }

private:
    InputType input_type_;
    GrowableByteArrayDataOutput buffer_;
};

} // namespace

FstBuilder::FstBuilder(const FstOutputs &outputs, InputType input_type)
    : outputs_(&outputs), input_type_(input_type), root_(std::make_unique<Node>()) {
    root_->output = outputs.get_no_output();
}

FstBuilder::~FstBuilder() = default;
FstBuilder::FstBuilder(FstBuilder &&) noexcept = default;
FstBuilder &FstBuilder::operator=(FstBuilder &&) noexcept = default;

FstBuilder::Node *FstBuilder::child_for(Node &node, int32_t label) {
    for (auto &arc : node.arcs) {
        if (arc.label == label) {
            return arc.child;
        }
    }
    auto child = std::make_unique<Node>();
    child->output = outputs_->get_no_output();
    Node *raw = child.get();
    owned_.push_back(std::move(child));
    node.arcs.push_back(ArcEntry{label, raw, outputs_->get_no_output()});
    return raw;
}

Result<bool> FstBuilder::add(std::span<const int32_t> key, std::any output) {
    if (finished_) {
        return err(LuceneError::illegal_state("add() after finish()"));
    }
    Node *node = root_.get();
    for (const int32_t label : key) {
        node = child_for(*node, label);
    }
    const bool was_final = node->is_final;
    node->is_final = true;
    node->final_output = output;
    return !was_final;
}

void FstBuilder::compute_outputs(Node &node, bool is_root) {
    if (!node.has_arcs()) {
        debug_check(node.is_final, "a node without arcs must accept a key");
        node.output = node.final_output;
        return;
    }
    std::any common_output;
    bool first = true;
    for (auto &arc : node.arcs) {
        compute_outputs(*arc.child, false);
        if (first) {
            common_output = arc.child->output;
            first = false;
        } else {
            common_output = outputs_->common(common_output, arc.child->output);
        }
    }
    if (node.is_final) {
        // A node may accept a key *and* have children ("ab" plus "abc").
        common_output = outputs_->common(common_output, node.final_output);
    }
    // The root's accumulated output is always NO_OUTPUT: readers seed their
    // accumulator with NO_OUTPUT and never add the root's own output, so
    // pushing a common prefix down to the root would lose it.
    node.output = is_root ? outputs_->get_no_output() : common_output;

    // Each arc carries only the part of its child's output that is not already
    // accumulated on the way to this node.
    for (auto &arc : node.arcs) {
        arc.output = outputs_->subtract(arc.child->output, node.output);
    }
}

int64_t FstBuilder::serialize(Node &node) {
    if (!node.has_arcs()) {
        // Virtual end nodes are never written.
        node.address = node.is_final ? FINAL_END_NODE : NON_FINAL_END_NODE;
        return node.address;
    }

    // Children first: a parent's address must be greater than its children's.
    for (auto &arc : node.arcs) {
        (void)serialize(*arc.child);
    }

    NodeBytes bytes(input_type_);
    for (size_t i = 0; i < node.arcs.size(); ++i) {
        const ArcEntry &arc = node.arcs[i];
        const Node &child = *arc.child;
        const bool child_has_arcs = child.has_arcs();

        uint8_t flags = 0;
        if (child.is_final) {
            flags |= BIT_FINAL_ARC;
        }
        if (i + 1 == node.arcs.size()) {
            flags |= BIT_LAST_ARC;
        }
        if (!child_has_arcs) {
            flags |= BIT_STOP_NODE;
        }
        const bool has_output = !outputs_->is_no_output(arc.output);
        if (has_output) {
            flags |= BIT_ARC_HAS_OUTPUT;
        }

        // The output that applies when the path ends at the child node.
        std::any next_final_output = outputs_->get_no_output();
        bool has_final_output = false;
        if (child.is_final) {
            next_final_output = outputs_->subtract(child.final_output, child.output);
            has_final_output = !outputs_->is_no_output(next_final_output);
            if (has_final_output) {
                flags |= BIT_ARC_HAS_FINAL_OUTPUT;
            }
        }

        bytes.write_flags(flags);
        if (auto result = bytes.write_label(arc.label); !result.has_value()) {
            debug_check(false, "writing a label cannot fail for an in-memory buffer");
        }
        if (has_output) {
            if (auto result = outputs_->write(arc.output, bytes.buffer()); !result.has_value()) {
                debug_check(false, "writing an output cannot fail for an in-memory buffer");
            }
        }
        if (has_final_output) {
            if (auto result = outputs_->write_final_output(next_final_output, bytes.buffer()); !result.has_value()) {
                debug_check(false, "writing a final output cannot fail for an in-memory buffer");
            }
        }
        if (!child_has_arcs) {
            // STOP_NODE: the target is implied by the final flag.
        } else if (auto result = bytes.write_target(child.address); !result.has_value()) {
            debug_check(false, "writing a target cannot fail for an in-memory buffer");
        }
    }

    const auto &written = bytes.buffer().bytes();
    std::string signature(reinterpret_cast<const char *>(written.data()), written.size());
    if (auto existing = node_hash_.find(signature); existing != node_hash_.end()) {
        // The same subtree was already written; share its address.
        node.address = existing->second;
        return node.address;
    }

    // The FST format stores every node **byte reversed** and addresses it at its
    // last byte: readers walk addresses downwards, so the node's first logical
    // byte must sit at the highest address.  Writing the arcs forward and
    // addressing the first byte produces a file that only reads correctly with a
    // forward reader — other implementations reject it.
    data_.insert(data_.end(), written.rbegin(), written.rend());
    node.address = static_cast<int64_t>(data_.size()) - 1;
    node_hash_.emplace(std::move(signature), node.address);
    return node.address;
}

Result<FstBuilder::BuildResult> FstBuilder::finish() {
    if (finished_) {
        return err(LuceneError::illegal_state("finish() called twice"));
    }
    finished_ = true;

    compute_outputs(*root_, true);

    // Address 0 is reserved for `NON_FINAL_END_NODE`, so the data starts with a
    // padding byte.
    data_.assign(1, 0);
    const int64_t start_node = serialize(*root_);

    FSTMetadata metadata;
    metadata.input_type = input_type_;
    metadata.outputs = outputs_;
    metadata.version = VERSION_CURRENT;
    metadata.start_node = root_->has_arcs() ? start_node : NON_FINAL_END_NODE;
    if (root_->is_final) {
        metadata.empty_output = root_->final_output;
    }
    metadata.num_bytes = static_cast<int64_t>(data_.size());

    BuildResult result;
    result.data = std::move(data_);
    result.metadata = metadata;
    return result;
}

} // namespace lucene
