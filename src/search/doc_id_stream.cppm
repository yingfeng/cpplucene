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

export module lucene_core:doc_id_stream;

import :doc_id_set_iterator;
import :error;
import :scorable;
import :types;

import std;

namespace lucene {

/// Consumes the documents of a [`DocIdStream`].
///
/// Port of `DocIdStreamConsumer` (`search/doc_id_stream.rs`).
export class DocIdStreamConsumer {
public:
    virtual ~DocIdStreamConsumer() = default;

    /// Consumes `doc` without a score.
    [[nodiscard]] virtual VoidResult accept(int32_t doc) = 0;

    /// Consumes `doc` with `score`; the consumer has to hand the score to
    /// whatever it collects into.
    [[nodiscard]] virtual VoidResult accept_with_score(int32_t doc, float score) = 0;
};

/// A stream of document ids, in increasing order.
///
/// Port of `DocIdStream` (`search/doc_id_stream.rs`).  All of its operations are
/// terminal: a stream must be consumed immediately and is not reusable.
export class DocIdStream {
public:
    virtual ~DocIdStream() = default;

    /// Calls `consumer` for every document, in increasing order.
    [[nodiscard]] virtual VoidResult for_each(DocIdStreamConsumer &consumer) = 0;

    /// Number of documents in the stream.
    ///
    /// **Deviation:** the reference implementation takes the scorer so the
    /// default implementation can set the score of every document; this port's
    /// [`Scorable`](:scorable) is a reference rather than a generic parameter, so
    /// the scorer is passed the same way and the default stays equivalent.
    [[nodiscard]] virtual Result<int32_t> count(Scorable &scorer);
};

/// A [`DocIdStream`] over a [`DocIdSetIterator`], e.g. the matches of a scorer
/// between two window boundaries.
///
/// **Deviation:** the reference implementation builds such a stream inside
/// `BooleanScorer` (`DocIdStreamView`) and keeps it private; this port exposes it
/// because it is the only producer the ported collectors can be handed, and the
/// windowed boolean scorer itself is not ported (see the `LeafCollector` note).
export class DocIdSetIteratorStream final : public DocIdStream {
public:
    /// Streams the documents of `iterator` that are `>= min` and `< max`.
    DocIdSetIteratorStream(DocIdSetIterator &iterator, int32_t min, int32_t max)
        : iterator_(&iterator), max_(max) {
        (void)min;
    }

    [[nodiscard]] VoidResult for_each(DocIdStreamConsumer &consumer) override;

    [[nodiscard]] Result<int32_t> count(Scorable &scorer) override;

    /// Moves the window end; the document the previous window stopped at is
    /// served first, so a leaf can be walked window by window with one stream.
    void set_max(int32_t max) noexcept { max_ = max; }
    [[nodiscard]] int32_t max() const noexcept { return max_; }

private:
    /// The document the previous window peeked at and did not consume.
    [[nodiscard]] Result<std::optional<int32_t>> next_doc();

    DocIdSetIterator *iterator_;
    int32_t max_;
    std::optional<int32_t> pending_;
};

/// A consumer that counts the documents it sees and, when a score is provided,
/// records it in the scorer.
///
/// Port of `CountConsumer` (`search/doc_id_stream.rs`).
export class CountConsumer final : public DocIdStreamConsumer {
public:
    explicit CountConsumer(Scorable &scorer) : scorer_(&scorer) {}

    [[nodiscard]] VoidResult accept(int32_t /*doc*/) override {
        ++count_;
        return {};
    }

    [[nodiscard]] VoidResult accept_with_score(int32_t /*doc*/, float score) override {
        ++count_;
        return scorer_->set_score(score);
    }

    [[nodiscard]] int32_t count() const noexcept { return count_; }

private:
    Scorable *scorer_;
    int32_t count_{0};
};

} // namespace lucene
