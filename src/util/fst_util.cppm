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

export module lucene_core:fst_util;

import :types;
import :error;

import std;

import :bytes_ref;
import :bytes_ref_builder;
import :fst;
import :fst_bytes_reader;
import :fst_outputs;
import :ints_ref;

namespace lucene {

/// Compares two FST outputs: negative when the first is smaller, zero when
/// they are equal, positive otherwise.
///
/// **Deviation:** rlucene makes the comparator a type parameter of
/// `TopNSearcher`.  Here the output value is already type erased as `std::any`,
/// so the comparator is likewise erased into a `std::function`.
export using OutputComparator = std::function<int32_t(const std::any &, const std::any &)>;

/// One completed search result: the input key plus the output it produces.
///
/// Port of `TopResult<T>` (`util/fst_impl/util.rs`).
export struct TopResult {
    IntsRef input;
    std::any output;
};

/// The (possibly incomplete) result of a [`TopNSearcher`] run.
export struct TopResults {
    /// `true` when the queue was deep enough to see every competitive path.
    bool is_complete{true};
    std::vector<TopResult> top_n;
};

/// One partially explored path.
///
/// Port of `FSTPath<T>`.  `arc` is the arc that was last appended to `input`.
export class FSTPath {
public:
    FSTPath() = default;
    FSTPath(std::any output, const Arc &arc, IntsRefBuilder input, float boost, std::string context, int32_t payload);

    /// A copy of this path with a different output and input.
    [[nodiscard]] FSTPath new_path(std::any output, IntsRefBuilder input) const;

    [[nodiscard]] std::string to_string() const;

    Arc arc;
    std::any output;
    IntsRefBuilder input;
    float boost{0.0F};
    std::string context;
    int32_t payload{-1};
};

/// Compares two complete or partial paths.
export using PathComparator = std::function<int32_t(const FSTPath &, const FSTPath &)>;

/// Hook that lets callers prune the search.
///
/// Port of the `TopNSearcherBase<T>` trait.  All predicates default to `true`.
export class TopNSearcherBase {
public:
    virtual ~TopNSearcherBase() = default;

    /// Called for a path whose last arc is the virtual END_LABEL arc.
    [[nodiscard]] virtual bool accept_result_path(const FSTPath &path) { return accept_result(path.input, path.output); }

    /// Called before a path is queued; returning `false` drops the subtree.
    [[nodiscard]] virtual bool accept_partial_path(const FSTPath &path) {
        (void)path;
        return true;
    }

    /// Called for a finished result.
    [[nodiscard]] virtual bool accept_result(const IntsRefBuilder &input, const std::any &output) {
        (void)input;
        (void)output;
        return true;
    }
};

/// Finds the top N shortest (cheapest) paths leaving one or more nodes.
///
/// Port of `TopNSearcher` (`util/fst_impl/util.rs`).  The searcher walks the
/// FST through `fst.reader()`, so the owning `FST` must outlive it and must not
/// be traversed concurrently.
export class TopNSearcher {
public:
    /// A searcher that breaks ties by comparing the inputs lexicographically.
    [[nodiscard]] static Result<TopNSearcher> by_tie_break(const FST &fst, size_t top_n, size_t max_queue_depth,
                                                           OutputComparator comparator);

    TopNSearcher(const FST &fst, size_t top_n, size_t max_queue_depth, OutputComparator comparator,
                 PathComparator path_comparator);

    /// Installs a pruning hook; the searcher takes ownership.
    void set_base(std::unique_ptr<TopNSearcherBase> base) { base_ = std::move(base); }

    /// Adds `path` to the queue when it can still win a slot.
    [[nodiscard]] VoidResult add_if_competitive(FSTPath &path);

    /// Queues every arc leaving `node`, including the final (END_LABEL) arc.
    [[nodiscard]] VoidResult add_start_paths(const Arc &node, std::any start_output, bool allow_empty_string,
                                             IntsRefBuilder input);

    /// [`add_start_paths`] with the suggester bookkeeping fields.
    [[nodiscard]] VoidResult add_start_paths_with_context(const Arc &node, std::any start_output,
                                                          bool allow_empty_string, IntsRefBuilder input, float boost,
                                                          std::string context, int32_t payload);

    /// Drains the queue, returning up to `top_n` paths in comparator order.
    [[nodiscard]] Result<TopResults> search();

    [[nodiscard]] bool is_queue_open() const noexcept { return queue_.has_value(); }

private:
    [[nodiscard]] VoidResult insert_queue(FSTPath path);
    [[nodiscard]] int32_t compare_inputs(const IntsRefBuilder &a, const IntsRefBuilder &b) const;

    const FST *fst_{nullptr};
    FstBytesReader *reader_{nullptr};
    size_t top_n_{0};
    size_t max_queue_depth_{0};
    Arc scratch_arc_{};
    OutputComparator comparator_;
    PathComparator path_comparator_;
    std::unique_ptr<TopNSearcherBase> base_;
    std::optional<std::deque<FSTPath>> queue_;
};

/// Helpers for looking values up in, and dumping, an [`FST`].
///
/// Port of `Util` (`util/fst_impl/util.rs`).
export struct FstUtil {
    /// The output for `input`, or `std::nullopt` when the FST does not accept it.
    [[nodiscard]] static Result<std::optional<std::any>> get_from_ints(const FST &fst,
                                                                        std::span<const int32_t> input);

    /// [`get_from_ints`] for a byte-oriented (BYTE1) FST.
    [[nodiscard]] static Result<std::optional<std::any>> get_from_bytes(const FST &fst, const BytesRef &input);

    /// Reads the first arc leaving `follow` whose label is `>= label`.
    ///
    /// Returns `false` when no such arc exists; `arc` then holds the position a
    /// caller like `seekFloor` needs to resume from.
    [[nodiscard]] static Result<bool> read_ceil_arc(int32_t label, const FST &fst, const Arc &follow, Arc &arc);

    /// Binary search inside a fixed-length arc array.
    ///
    /// Returns the index of the matching arc, or `-1 - idx` where `idx` is the
    /// index of the next higher label (`num_arcs` when the label is too big).
    [[nodiscard]] static Result<int32_t> binary_search(const FST &fst, const Arc &arc, int32_t target_label);

    /// The `top_n` cheapest completions reachable from `from_node`.
    [[nodiscard]] static Result<TopResults> shortest_paths(const FST &fst, const Arc &from_node,
                                                            std::any start_output, OutputComparator comparator,
                                                            size_t top_n, bool allow_empty_string);

    /// Renders the FST in GraphViz' dot language, appended to `out`.
    ///
    /// `same_rank` asks GraphViz to lay out each breadth-first level on one
    /// rank; `label_states` labels every state with its serialized address.
    [[nodiscard]] static VoidResult to_dot(const FST &fst, std::string &out, bool same_rank, bool label_states);

    /// Splits `text` (UTF-8) into Unicode code points.
    [[nodiscard]] static VoidResult to_utf32(std::string_view text, IntsRefBuilder &scratch);

    /// Widens every byte of `input` into an `int32_t` label.
    [[nodiscard]] static VoidResult to_ints_ref(const BytesRef &input, IntsRefBuilder &scratch);

    /// Narrows every label of `input` into a byte; labels outside `[0, 255]`
    /// are a programming error (`debug_check`).
    [[nodiscard]] static Result<BytesRef> to_bytes_ref(const IntsRef &input, BytesRefBuilder &scratch);
};

} // namespace lucene
