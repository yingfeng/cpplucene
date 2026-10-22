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

export module lucene_core:leaf_reader_context;

import :types;
import std;

import :identity;

namespace lucene {

export class LeafReader;

/// Metadata of the top-level reader a leaf context belongs to.
///
/// Port of `TopParentMeta` (`core/index/leaf_reader_context.rs`).  Every leaf of
/// one reader tree shares the same `id`, which is what lets a `Weight` recognise
/// that it is being used with the reader its term states were built for.
export struct TopParentMeta {
    /// Number of leaves below the top-level reader.
    int32_t leaves_num{0};
    /// `max_doc` of the top-level reader.
    int32_t max_doc{0};
    /// Identity of the top-level reader.
    Identity id;
};

/// Where one leaf reader sits inside its reader tree.
///
/// Port of `LeafReaderContext` (`core/index/leaf_reader_context.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reader is a borrowed pointer: the reader tree owns its leaves, and a
///    context never outlives the tree it came from;
///  * `doc_base` / `ord` use `int32_t`, Lucene's own document-number type
///    (rlucene uses `usize`).
export class LeafReaderContext {
public:
    LeafReaderContext(LeafReader *reader, int32_t ord, int32_t doc_base, TopParentMeta top_parent)
        : reader_(reader), ord_(ord), doc_base_(doc_base), top_parent_(std::move(top_parent)) {}

    /// The leaf reader; never null.
    [[nodiscard]] LeafReader *reader() const noexcept { return reader_; }

    /// The reader's ord in the top-level's leaves array.
    [[nodiscard]] int32_t ord() const noexcept { return ord_; }

    /// The reader's absolute doc base: add it to a leaf-local doc id to obtain
    /// the id the top-level reader uses.
    [[nodiscard]] int32_t doc_base() const noexcept { return doc_base_; }

    /// Metadata of the top-level reader this leaf belongs to.
    [[nodiscard]] const TopParentMeta &top_parent() const noexcept { return top_parent_; }

    [[nodiscard]] std::string to_string() const {
        return "LeafReaderContext(doc_base=" + std::to_string(doc_base_) + " ord=" + std::to_string(ord_) + ")";
    }

private:
    LeafReader *reader_;
    int32_t ord_;
    int32_t doc_base_;
    TopParentMeta top_parent_;
};

} // namespace lucene
