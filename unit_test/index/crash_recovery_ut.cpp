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

// Crash recovery: a child process kills itself with SIGKILL in the middle of a
// write, and the parent then has to find a consistent index.  The point is the
// commit contract: everything up to the last commit survives, everything after it
// may be gone, and the index stays readable, checkable and writable.
//
// `SIGKILL` is POSIX only, so the test skips elsewhere.

module;

#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define CLUCENE_HAS_FORK 1
#else
#define CLUCENE_HAS_FORK 0
#endif

module lucene_core:ut.crash_recovery;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kCommittedDocs = 40;
constexpr int32_t kUncommittedDocs = 60;

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

Document doc_of(int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("body", FieldValue{std::string("doc") + std::to_string(id)}, body_type()));
    return doc;
}

/// Opens an index at `path`, writes `first` documents, commits, then writes
/// `second` more without committing and kills the process.
[[noreturn]] void write_and_die(const std::filesystem::path &path) {
    auto directory = FSDirectory::open(path);
    if (!directory.has_value()) {
        std::_Exit(2);
    }
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    if (!writer.has_value()) {
        std::_Exit(3);
    }
    for (int32_t i = 0; i < kCommittedDocs; ++i) {
        if (!writer.value()->add_document(doc_of(i)).has_value()) {
            std::_Exit(4);
        }
    }
    if (!writer.value()->commit().has_value()) {
        std::_Exit(5);
    }
    // These are buffered, never committed.
    for (int32_t i = 0; i < kUncommittedDocs; ++i) {
        if (!writer.value()->add_document(doc_of(1000 + i)).has_value()) {
            std::_Exit(6);
        }
    }
    // Hand the buffers to the writer's own flush path so that the kill lands while
    // segment files exist but the commit point does not.
    (void)writer.value()->flush();

    std::raise(SIGKILL);
    std::_Exit(7); // not reached
}

} // namespace

#if CLUCENE_HAS_FORK

TEST(CrashRecoveryTest, SurvivesAKillWhileWriting) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_crash_recovery";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path, ignored);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0) << "fork failed";
    if (child == 0) {
        write_and_die(path);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status)) << "the child was expected to be killed";
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    // 1. The committed documents are all there, the uncommitted ones are gone.
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());
    {
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
        EXPECT_EQ(reader.value()->num_docs().value(), kCommittedDocs);
        EXPECT_TRUE(reader.value()->close().has_value());
    }

    // 2. The on-disk structures are consistent.
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked.has_value() ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();

    // 3. The index can be opened for writing again: the process death released the
    //    write lock and the pending (uncommitted) files are simply ignored.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        for (int32_t i = 0; i < 10; ++i) {
            ASSERT_TRUE(writer.value()->add_document(doc_of(2000 + i)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reopened = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reopened.has_value()) << (reopened.has_value() ? "" : reopened.error().to_string());
    EXPECT_EQ(reopened.value()->num_docs().value(), kCommittedDocs + 10);
    EXPECT_TRUE(reopened.value()->close().has_value());
}

TEST(CrashRecoveryTest, SurvivesAKillBeforeTheFirstCommit) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_crash_recovery_empty";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path, ignored);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto directory = FSDirectory::open(path);
        if (!directory.has_value()) {
            std::_Exit(2);
        }
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        if (!writer.has_value()) {
            std::_Exit(3);
        }
        for (int32_t i = 0; i < 25; ++i) {
            (void)writer.value()->add_document(doc_of(i));
        }
        (void)writer.value()->flush();
        std::raise(SIGKILL);
        std::_Exit(7);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));

    // Without a commit there is no commit point at all, so the directory is not an
    // index yet: reading it reports the same "index not found" the reference
    // implementation throws, instead of a half-written one.
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    EXPECT_FALSE(reader.has_value());
    if (!reader.has_value()) {
        EXPECT_NE(reader.error().to_string().find("no segments"), std::string::npos) << reader.error().to_string();
    }

    // Writing again creates a fresh index; the documents that were never committed
    // are gone.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(doc_of(1)).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    auto reopened = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened.value()->num_docs().value(), 1);
    EXPECT_TRUE(reopened.value()->close().has_value());

    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked.has_value() ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
}

#else

TEST(CrashRecoveryTest, SkipsWithoutFork) { GTEST_SKIP() << "SIGKILL based recovery needs a POSIX fork"; }

#endif
