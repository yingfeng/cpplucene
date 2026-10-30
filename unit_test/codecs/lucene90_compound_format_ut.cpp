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
#include <span>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.lucene90_compound_format;

import lucene_core;

using namespace lucene;

namespace {

SegmentInfo segment_for(std::string name = "_0") {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x7F);
    info.max_doc = 3;
    return info;
}

/// Writes a sub-file the way a codec does: index header, payload, footer.
void write_sub_file(Directory &directory, const SegmentInfo &si, std::string_view file, std::string_view payload) {
    auto out = directory.create_output(file, io_context_default());
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(CodecUtil::write_index_header(*out.value(), "TestCodec", 0, si.id, "").has_value());
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    ASSERT_TRUE(out.value()->write_bytes(bytes).has_value());
    ASSERT_TRUE(CodecUtil::write_footer(*out.value()).has_value());
    ASSERT_TRUE(out.value()->close().has_value());
}

/// Reads a sub-file back through the compound directory.
std::string read_sub_file(CompoundDirectory &directory, const SegmentInfo &si, std::string_view name) {
    auto input = directory.open_input(name, io_context_default());
    EXPECT_TRUE(input.has_value());
    auto checked = CodecUtil::check_index_header(*input.value(), "TestCodec", 0, 0, si.id, "");
    EXPECT_TRUE(checked.has_value());

    auto length = input.value()->length();
    EXPECT_TRUE(length.has_value());
    auto position = input.value()->get_file_pointer();
    EXPECT_TRUE(position.has_value());
    const size_t payload_length = length.value() - CodecUtil::footer_length() - position.value();

    std::vector<uint8_t> payload(payload_length);
    EXPECT_TRUE(input.value()->read_bytes(payload).has_value());
    EXPECT_TRUE(CodecUtil::check_footer(*input.value()).has_value());
    return std::string(payload.begin(), payload.end());
}

} // namespace

TEST(Lucene90CompoundFormatTest, RoundTripsSubFiles) {
    ByteBuffersDirectory directory;
    SegmentInfo si = segment_for();
    si.files = {"_0_Lucene90_0.tim", "_0_Lucene90_0.doc", "_0_Lucene90_0.nvd"};
    write_sub_file(directory, si, "_0_Lucene90_0.tim", "terms-and-postings");
    write_sub_file(directory, si, "_0_Lucene90_0.doc", "stored-fields-payload");
    write_sub_file(directory, si, "_0_Lucene90_0.nvd", "norms");

    Lucene90CompoundFormat format;
    ASSERT_TRUE(format.write(directory, si, io_context_default()).has_value());

    // The compound files exist next to the originals.
    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::ranges::find(names.value(), "_0.cfs"), names.value().end());
    EXPECT_NE(std::ranges::find(names.value(), "_0.cfe"), names.value().end());

    // `entries()` is introspective API of the concrete reader, so open it
    // directly (the format returns it through the `CompoundDirectory` view).
    auto concrete = Lucene90CompoundReader::create(directory, si);
    ASSERT_TRUE(concrete.has_value());
    ASSERT_EQ(concrete.value()->entries().size(), 3u);
    // Entry ids are the file name without its segment name (the part from the
    // second `_` on), the spelling the reference implementation writes, so the
    // `.cfs` stays interchangeable.
    EXPECT_TRUE(concrete.value()->entries().contains("_Lucene90_0.tim"));
    EXPECT_TRUE(concrete.value()->entries().contains("_Lucene90_0.doc"));
    EXPECT_TRUE(concrete.value()->entries().contains("_Lucene90_0.nvd"));

    auto reader = format.get_compound_reader(directory, si);
    ASSERT_TRUE(reader.has_value());

    // Sub-files keep their content, readable through the virtual directory.
    EXPECT_EQ(read_sub_file(*reader.value(), si, "_0_Lucene90_0.tim"), "terms-and-postings");
    EXPECT_EQ(read_sub_file(*reader.value(), si, "_0_Lucene90_0.doc"), "stored-fields-payload");
    EXPECT_EQ(read_sub_file(*reader.value(), si, "_0_Lucene90_0.nvd"), "norms");

    // The packed entry keeps the whole file (header, payload and footer), so its
    // length matches the original.
    auto packed_length = reader.value()->file_length("_0_Lucene90_0.nvd");
    ASSERT_TRUE(packed_length.has_value());
    auto original_length = directory.file_length("_0_Lucene90_0.nvd");
    ASSERT_TRUE(original_length.has_value());
    EXPECT_EQ(packed_length.value(), original_length.value());

    EXPECT_TRUE(reader.value()->check_integrity().has_value());
    EXPECT_EQ(reader.value()->to_string(), "CompoundFileDirectory(segment=\"_0\" in dir=" + directory.to_string() + ")");
}

TEST(Lucene90CompoundFormatTest, ListsSubFilesWithTheirSegmentPrefix) {
    ByteBuffersDirectory directory;
    SegmentInfo si = segment_for();
    si.files = {"_0_a.foo", "_0_b.bar"};
    write_sub_file(directory, si, "_0_a.foo", "a");
    write_sub_file(directory, si, "_0_b.bar", "bb");

    Lucene90CompoundFormat format;
    ASSERT_TRUE(format.write(directory, si, io_context_default()).has_value());
    auto reader = format.get_compound_reader(directory, si);
    ASSERT_TRUE(reader.has_value());

    auto listed = reader.value()->list_all();
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed.value(), (std::vector<std::string>{"_0_a.foo", "_0_b.bar"}));
}

TEST(Lucene90CompoundFormatTest, EntryIdsDropOnlyTheSegmentName) {
    // A per-segment file has a single `_` (the segment name is the whole prefix
    // before the first `.`), a codec-suffixed one has two.  Both lose only the
    // segment name, so the suffix survives in the entry id.
    ByteBuffersDirectory directory;
    SegmentInfo si = segment_for();
    si.files = {"_0.fnm", "_0_Lucene90_0.dvd"};
    write_sub_file(directory, si, "_0.fnm", "field-infos");
    write_sub_file(directory, si, "_0_Lucene90_0.dvd", "doc-values");

    Lucene90CompoundFormat format;
    ASSERT_TRUE(format.write(directory, si, io_context_default()).has_value());

    auto concrete = Lucene90CompoundReader::create(directory, si);
    ASSERT_TRUE(concrete.has_value());
    ASSERT_EQ(concrete.value()->entries().size(), 2u);
    EXPECT_TRUE(concrete.value()->entries().contains(".fnm"));
    EXPECT_TRUE(concrete.value()->entries().contains("_Lucene90_0.dvd"));

    auto reader = format.get_compound_reader(directory, si);
    ASSERT_TRUE(reader.has_value());
    // Opening by the full name resolves through the same stripping rule.
    EXPECT_EQ(read_sub_file(*reader.value(), si, "_0.fnm"), "field-infos");
    EXPECT_EQ(read_sub_file(*reader.value(), si, "_0_Lucene90_0.dvd"), "doc-values");

    // Listing re-attaches the segment name, so the names match the directory.
    auto listed = reader.value()->list_all();
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed.value(), (std::vector<std::string>{"_0.fnm", "_0_Lucene90_0.dvd"}));
}

TEST(Lucene90CompoundFormatTest, IsReadOnly) {
    ByteBuffersDirectory directory;
    SegmentInfo si = segment_for();
    si.files = {"_0_a.foo"};
    write_sub_file(directory, si, "_0_a.foo", "a");

    Lucene90CompoundFormat format;
    ASSERT_TRUE(format.write(directory, si, io_context_default()).has_value());
    auto reader = format.get_compound_reader(directory, si);
    ASSERT_TRUE(reader.has_value());

    EXPECT_EQ(reader.value()->create_output("_0_x", io_context_default()).error().kind(),
              ErrorKind::kUnsupportedOperation);
    EXPECT_EQ(reader.value()->create_temp_output("x", "y", io_context_default()).error().kind(),
              ErrorKind::kUnsupportedOperation);
    EXPECT_EQ(reader.value()->delete_file("_0_a.foo").error().kind(), ErrorKind::kUnsupportedOperation);
    EXPECT_EQ(reader.value()->rename("_0_a.foo", "_0_b.foo").error().kind(), ErrorKind::kUnsupportedOperation);
    EXPECT_EQ(reader.value()->obtain_lock("write.lock").error().kind(), ErrorKind::kUnsupportedOperation);
    EXPECT_EQ(reader.value()->sync(std::vector<std::string>{"_0.cfs"}).error().kind(),
              ErrorKind::kUnsupportedOperation);
    EXPECT_TRUE(reader.value()->sync_metadata().has_value());

    // Missing sub-files are reported, not silently sliced.
    auto missing = reader.value()->open_input("_0_nope.foo", io_context_default());
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kNotSuchFile);
}

TEST(Lucene90CompoundFormatTest, RejectsTruncatedDataFile) {
    ByteBuffersDirectory directory;
    SegmentInfo si = segment_for();
    si.files = {"_0_a.foo"};
    write_sub_file(directory, si, "_0_a.foo", "payload");

    Lucene90CompoundFormat format;
    ASSERT_TRUE(format.write(directory, si, io_context_default()).has_value());

    // Rewrite the .cfs without its last bytes, the way a truncated copy looks.
    auto original = directory.open_input("_0.cfs");
    ASSERT_TRUE(original.has_value());
    auto length = original.value()->length();
    ASSERT_TRUE(length.has_value());
    std::vector<uint8_t> bytes(length.value() - 8);
    ASSERT_TRUE(original.value()->read_bytes(bytes).has_value());
    EXPECT_TRUE(original.value()->close().has_value());

    auto truncated = directory.create_output("_0.cfs", io_context_default());
    ASSERT_TRUE(truncated.has_value());
    ASSERT_TRUE(truncated.value()->write_bytes(bytes).has_value());
    ASSERT_TRUE(truncated.value()->close().has_value());

    auto reader = format.get_compound_reader(directory, si);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kCorruptIndex);
}
