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
#include <expected>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.two_phase_commit;

import lucene_core;

using namespace lucene;

namespace {

/// A participant that records the calls it receives.
class RecordingParticipant final : public TwoPhaseCommit {
public:
    RecordingParticipant(std::vector<std::string> &log, std::string name, int64_t generation,
                         std::optional<LuceneError> failure = std::nullopt)
            : log_(log), name_(std::move(name)), generation_(generation), failure_(std::move(failure)) {}

    [[nodiscard]] Result<int64_t> prepare_commit() override {
        log_.push_back(name_ + ":prepare");
        if (failure_.has_value()) {
            return std::unexpected(failure_.value());
        }
        return generation_;
    }

    [[nodiscard]] Result<int64_t> commit() override {
        log_.push_back(name_ + ":commit");
        if (failure_.has_value()) {
            return std::unexpected(failure_.value());
        }
        return generation_;
    }

    [[nodiscard]] VoidResult rollback() override {
        log_.push_back(name_ + ":rollback");
        return {};
    }

private:
    std::vector<std::string> &log_;
    std::string name_;
    int64_t generation_;
    std::optional<LuceneError> failure_;
};

FieldType two_phase_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document two_phase_doc(std::string id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, two_phase_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::string("alpha")}, two_phase_type()));
    return doc;
}

/// The documents the committed index holds, or `-1` when it does not exist yet.
int64_t committed_docs(ByteBuffersDirectory &directory) {
    auto exists = DirectoryReader::index_exists(directory);
    if (!exists.has_value() || !exists.value()) {
        return -1;
    }
    auto reader = DirectoryReader::open(directory);
    if (!reader.has_value()) {
        return -2;
    }
    return reader.value()->num_docs().value();
}

} // namespace

TEST(TwoPhaseCommitTest, ParticipantsCanBeCoordinated) {
    std::vector<std::string> log;
    RecordingParticipant first(log, "a", 7);
    RecordingParticipant second(log, "b", 7);

    // The coordinator's contract: prepare everywhere, then commit everywhere.
    const std::vector<int64_t> prepared{first.prepare_commit().value(), second.prepare_commit().value()};
    EXPECT_EQ(prepared, (std::vector<int64_t>{7, 7}));

    const std::vector<int64_t> committed{first.commit().value(), second.commit().value()};
    EXPECT_EQ(committed, (std::vector<int64_t>{7, 7}));
    EXPECT_EQ(log, (std::vector<std::string>{"a:prepare", "b:prepare", "a:commit", "b:commit"}));
}

TEST(TwoPhaseCommitTest, AFailedPrepareRollsTheOthersBack) {
    std::vector<std::string> log;
    RecordingParticipant healthy(log, "healthy", 3);
    RecordingParticipant broken(log, "broken", 3, LuceneError::io("disk full"));

    ASSERT_TRUE(healthy.prepare_commit().has_value());
    auto failed = broken.prepare_commit();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIo);

    ASSERT_TRUE(healthy.rollback().has_value());
    EXPECT_EQ(log, (std::vector<std::string>{"healthy:prepare", "broken:prepare", "healthy:rollback"}));
}

// ---- the writer is a participant as well ----

TEST(TwoPhaseCommitWriterTest, PrepareDoesNotPublishAndCommitPreparedDoes) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("a")).has_value());
    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("b")).has_value());

    EXPECT_FALSE(writer.value()->has_prepared_commit());
    auto prepared = writer.value()->prepare_commit();
    ASSERT_TRUE(prepared.has_value()) << (prepared ? "" : prepared.error().to_string());
    EXPECT_TRUE(writer.value()->has_prepared_commit());
    // The first phase wrote the segment but not `segments_N`, so a reader still sees
    // the index as it was — empty, in this case.
    EXPECT_EQ(committed_docs(directory), -1) << "the prepared commit is not visible";

    auto published = writer.value()->commit_prepared();
    ASSERT_TRUE(published.has_value()) << (published ? "" : published.error().to_string());
    EXPECT_FALSE(writer.value()->has_prepared_commit());
    EXPECT_EQ(published.value(), prepared.value()) << "the commit landed in the prepared generation";
    EXPECT_EQ(committed_docs(directory), 2);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(TwoPhaseCommitWriterTest, RollbackDropsAPreparedCommit) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("a")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(committed_docs(directory), 1);

    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("b")).has_value());
    auto prepared = writer.value()->prepare_commit();
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(writer.value()->rollback().has_value());
    EXPECT_FALSE(writer.value()->has_prepared_commit());
    EXPECT_EQ(committed_docs(directory), 1) << "the prepared document never became visible";
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(TwoPhaseCommitWriterTest, PreparingTwiceIsIdempotent) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("a")).has_value());

    auto first = writer.value()->prepare_commit();
    ASSERT_TRUE(first.has_value());
    auto second = writer.value()->prepare_commit();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), first.value()) << "the second phase would land in the same generation";

    auto published = writer.value()->commit_prepared();
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published.value(), first.value());
    EXPECT_EQ(committed_docs(directory), 1);

    // Publishing again answers the same generation instead of writing a new one.
    auto again = writer.value()->commit_prepared();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), first.value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(TwoPhaseCommitWriterTest, APlainCommitPublishesAPreparedOne) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(two_phase_doc("a")).has_value());
    ASSERT_TRUE(writer.value()->prepare_commit().has_value());

    // `commit` finishes the commit that was prepared instead of preparing a second one.
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_FALSE(writer.value()->has_prepared_commit());
    EXPECT_EQ(committed_docs(directory), 1);
    ASSERT_TRUE(writer.value()->close().has_value());
}
