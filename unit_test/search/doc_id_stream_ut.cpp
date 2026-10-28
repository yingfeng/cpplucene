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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

module lucene_core:ut.doc_id_stream;

import lucene_core;

using namespace lucene;

namespace {

/// A scorer that remembers the last score it was given.
class RecordingScorable final : public Scorable {
public:
    [[nodiscard]] Result<float> score() override { return last_score_; }

    [[nodiscard]] VoidResult set_score(float score) override {
        last_score_ = score;
        return {};
    }

    [[nodiscard]] float last_score() const noexcept { return last_score_; }

private:
    float last_score_{0.0F};
};

/// A collector that records the documents it was handed.
class RecordingCollector final : public LeafCollector {
public:
    [[nodiscard]] VoidResult collect(int32_t doc, Scorable & /*scorer*/) override {
        docs.push_back(doc);
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "RecordingCollector"; }

    std::vector<int32_t> docs;
};

/// A stream of the three documents of the test index.
std::unique_ptr<DocIdSetIterator> three_docs() {
    auto set = IntArrayDocIdSet::create(std::vector<int32_t>{2, 5, 9, NO_MORE_DOCS}, 3);
    EXPECT_TRUE(set.has_value());
    auto iterator = set.value()->iterator();
    EXPECT_TRUE(iterator.has_value());
    return std::move(iterator.value());
}

} // namespace

TEST(DocIdStreamTest, StreamsTheDocumentsOfAnIteratorUpToTheWindowEnd) {
    auto iterator = three_docs();
    DocIdSetIteratorStream stream(*iterator, 0, 100);

    std::vector<int32_t> docs;
    class CollectingConsumer final : public DocIdStreamConsumer {
    public:
        explicit CollectingConsumer(std::vector<int32_t> &docs) : docs_(&docs) {}
        [[nodiscard]] VoidResult accept(int32_t doc) override {
            docs_->push_back(doc);
            return {};
        }
        [[nodiscard]] VoidResult accept_with_score(int32_t doc, float /*score*/) override {
            docs_->push_back(doc);
            return {};
        }

    private:
        std::vector<int32_t> *docs_;
    };
    CollectingConsumer consumer(docs);
    ASSERT_TRUE(stream.for_each(consumer).has_value());
    EXPECT_EQ(docs, (std::vector<int32_t>{2, 5, 9}));
}

TEST(DocIdStreamTest, StopsAtTheWindowEndAndTheIteratorKeepsItsPosition) {
    auto iterator = three_docs();
    // Only the documents below 6 belong to this window.  The document that ended
    // the window (9) is kept and served by the next one.
    DocIdSetIteratorStream window(*iterator, 0, 6);
    RecordingScorable scorer;
    EXPECT_EQ(window.count(scorer).value(), 2);

    window.set_max(100);
    EXPECT_EQ(window.count(scorer).value(), 1);
    // A third window finds nothing.
    window.set_max(100);
    EXPECT_EQ(window.count(scorer).value(), 0);
}

TEST(DocIdStreamTest, TheDefaultCountConsumesTheStream) {
    auto iterator = three_docs();
    DocIdSetIteratorStream stream(*iterator, 0, 100);
    RecordingScorable scorer;
    // `DocIdSetIteratorStream::count` does not need scores, but the default
    // implementation on any other stream would go through `CountConsumer`.
    EXPECT_EQ(stream.count(scorer).value(), 3);
    EXPECT_EQ(scorer.last_score(), 0.0F);
}

TEST(DocIdStreamTest, CountConsumerForwardsTheScore) {
    RecordingScorable scorer;
    CountConsumer consumer(scorer);
    ASSERT_TRUE(consumer.accept(1).has_value());
    EXPECT_EQ(consumer.count(), 1);
    EXPECT_EQ(scorer.last_score(), 0.0F);

    ASSERT_TRUE(consumer.accept_with_score(2, 3.5F).has_value());
    EXPECT_EQ(consumer.count(), 2);
    EXPECT_EQ(scorer.last_score(), 3.5F);
}

TEST(DocIdStreamTest, TheDefaultCollectStreamFallsBackToPerDocumentCollect) {
    auto iterator = three_docs();
    DocIdSetIteratorStream stream(*iterator, 0, 100);
    RecordingScorable scorer;
    RecordingCollector collector;
    ASSERT_TRUE(collector.collect_stream(stream, scorer).has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{2, 5, 9}));
}

TEST(DocIdStreamTest, ACustomStreamCanFeedTheCollector) {
    // A stream that hands out two documents and then stops, to show that the
    // collector does not depend on the iterator-backed stream.
    class TwoDocStream final : public DocIdStream {
    public:
        [[nodiscard]] VoidResult for_each(DocIdStreamConsumer &consumer) override {
            if (auto first = consumer.accept_with_score(4, 1.5F); !first.has_value()) {
                return first;
            }
            return consumer.accept_with_score(8, 2.5F);
        }
    };

    TwoDocStream stream;
    RecordingScorable scorer;
    RecordingCollector collector;
    ASSERT_TRUE(collector.collect_stream(stream, scorer).has_value());
    EXPECT_EQ(collector.docs, (std::vector<int32_t>{4, 8}));
    EXPECT_EQ(scorer.last_score(), 2.5F);
    EXPECT_EQ(stream.count(scorer).value(), 2);
}
