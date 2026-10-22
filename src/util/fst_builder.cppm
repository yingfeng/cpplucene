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

export module lucene_core:fst_builder;

import :types;
import :error;

import std;

import :fst;
import :fst_outputs;

namespace lucene {

/// Builds an FST from a stream of `(key, output)` pairs.
///
/// This is the C++ replacement for rlucene's `FSTCompiler`
/// (`util/fst_impl/fst_compiler.rs`, 1544 lines).  It produces a **valid Lucene
/// FST** — the file is readable by [`FST`] and by Java Lucene — but deliberately
/// implements only the variable-length arc layout:
///
///  * keys must be added in **strictly increasing lexicographic order**;
///  * nodes are serialized bottom-up (children first), so a parent's address is
///    always greater than its children's;
///  * a single padding byte is emitted first so that no node ever gets address
///    0, which is reserved for `NON_FINAL_END_NODE`.
///
/// Identical subtrees are serialized **once**: every finished node is looked up
/// by its serialized bytes, and a hit reuses the earlier address (the `NodeHash`
/// de-duplication of `fst_compiler.rs`).  Without it a term dictionary repeats
/// every shared suffix, which is most of the file.
///
/// **Not ported (pure size/speed optimizations, they do not change the
/// format's expressiveness):** the fixed-length arc layouts
/// (`ARCS_FOR_BINARY_SEARCH`, `ARCS_FOR_DIRECT_ADDRESSING`,
/// `ARCS_FOR_CONTINUOUS`) and the arc-expansion heuristics.  Those can be added
/// later without breaking readers.
export class FstBuilder {
public:
    /// The result of [`finish`].
    struct BuildResult {
        /// The FST data; the metadata's `start_node` is an offset into it.
        std::vector<uint8_t> data;
        FSTMetadata metadata;
    };

    explicit FstBuilder(const FstOutputs &outputs, InputType input_type = InputType::kByte1);
    ~FstBuilder();
    FstBuilder(const FstBuilder &) = delete;
    FstBuilder &operator=(const FstBuilder &) = delete;
    FstBuilder(FstBuilder &&) noexcept;
    FstBuilder &operator=(FstBuilder &&) noexcept;

    /// Adds one key with its output; keys must arrive in strictly increasing
    /// order.  Returns `false` when the key was already present (the last
    /// output wins, like Lucene's `add`).
    [[nodiscard]] Result<bool> add(std::span<const int32_t> key, std::any output);

    /// Serializes the automaton.  Must be called once, after all keys.
    [[nodiscard]] Result<BuildResult> finish();

private:
    struct Node;

    struct ArcEntry {
        int32_t label{0};
        Node *child{nullptr};
        std::any output{};
    };

    struct Node {
        std::vector<ArcEntry> arcs;
        bool is_final{false};
        std::any final_output{};
        std::any output{};
        int64_t address{NON_FINAL_END_NODE};

        [[nodiscard]] bool has_arcs() const noexcept { return !arcs.empty(); }
    };

    [[nodiscard]] Node *child_for(Node &node, int32_t label);
    void compute_outputs(Node &node, bool is_root);
    [[nodiscard]] int64_t serialize(Node &node);

    const FstOutputs *outputs_;
    InputType input_type_;
    std::unique_ptr<Node> root_;
    /// Owns every child node; `Node::arcs` only holds raw pointers.
    std::vector<std::unique_ptr<Node>> owned_;
    std::vector<uint8_t> data_;
    /// Serialized node bytes -> address of the already written copy.
    std::unordered_map<std::string, int64_t> node_hash_;
    bool finished_{false};
};

} // namespace lucene
