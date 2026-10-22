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
#include <string>

module lucene_core:ut.fst_metadata;

import lucene_core;

using namespace lucene;

namespace {

const BytesRef &as_bytes(const std::any &value) {
    static const BytesRef empty;
    if (value.type() != typeid(BytesRef)) {
        return empty;
    }
    return std::any_cast<const BytesRef &>(value);
}

/// A [`FstBytesReader`] over a buffer, used to exercise `FST::read_label`.
class BufferReader final : public FstBytesReader {
public:
    explicit BufferReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        return bytes_[static_cast<size_t>(pos_++)];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        for (auto &byte : b) {
            auto value = read_byte();
            if (!value.has_value()) {
                return err(value.error());
            }
            byte = value.value();
        }
        return {};
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        pos_ += num_bytes;
        return {};
    }

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

} // namespace

TEST(FstMetadataTest, RoundTripsWithAnEmptyOutput) {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FSTMetadata metadata;
    metadata.input_type = InputType::kByte2;
    metadata.outputs = &outputs;
    metadata.empty_output = std::any{BytesRef::from_string("empty-output")};
    metadata.start_node = 42;
    metadata.num_bytes = 123456;

    GrowableByteArrayDataOutput out;
    auto saved = metadata.save(out);
    ASSERT_TRUE(saved.has_value()) << (saved ? "" : saved.error().to_string());
    // header + "has empty output" byte + vint length + payload + type + 2 vlongs
    EXPECT_GT(out.length(), CodecUtil::header_length(FILE_FORMAT_NAME));

    ByteArrayDataInput in(out.bytes());
    auto restored = FSTMetadata::read(in, outputs);
    ASSERT_TRUE(restored.has_value()) << (restored ? "" : restored.error().to_string());
    EXPECT_EQ(restored->version, VERSION_CURRENT);
    EXPECT_EQ(restored->input_type, InputType::kByte2);
    EXPECT_EQ(restored->start_node, 42);
    EXPECT_EQ(restored->num_bytes, 123456);
    ASSERT_TRUE(restored->empty_output.has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(as_bytes(restored->empty_output.value()).as_slice().data()),
                          as_bytes(restored->empty_output.value()).length()),
              "empty-output");
    EXPECT_TRUE(in.eof());
}

TEST(FstMetadataTest, RoundTripsWithoutAnEmptyOutput) {
    const auto &outputs = FstPositiveIntOutputs::singleton();
    FSTMetadata metadata;
    metadata.input_type = InputType::kByte4;
    metadata.outputs = &outputs;
    metadata.start_node = 0;
    metadata.num_bytes = 0;

    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(metadata.save(out).has_value());

    ByteArrayDataInput in(out.bytes());
    auto restored = FSTMetadata::read(in, outputs);
    ASSERT_TRUE(restored.has_value()) << (restored ? "" : restored.error().to_string());
    EXPECT_FALSE(restored->empty_output.has_value());
    EXPECT_EQ(restored->input_type, InputType::kByte4);
    EXPECT_EQ(restored->start_node, 0);
    EXPECT_EQ(restored->num_bytes, 0);
}

TEST(FstMetadataTest, RejectsAForeignOrCorruptHeader) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    // A file written by another codec must not be read as an FST.
    GrowableByteArrayDataOutput other;
    ASSERT_TRUE(CodecUtil::write_header(other, "TIM", VERSION_CURRENT).has_value());
    ByteArrayDataInput wrong_codec(other.bytes());
    auto rejected = FSTMetadata::read(wrong_codec, outputs);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kCorruptIndex);

    // An invalid input type is reported as corruption.
    FSTMetadata metadata;
    metadata.outputs = &outputs;
    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(metadata.save(out).has_value());
    std::vector<uint8_t> bytes = out.bytes();
    // The input type byte sits right before the two vlongs; with start_node and
    // num_bytes both 0 it is the second to last byte.
    bytes[bytes.size() - 3] = 9;
    ByteArrayDataInput in(bytes);
    auto bad_type = FSTMetadata::read(in, outputs);
    ASSERT_FALSE(bad_type.has_value());
    EXPECT_EQ(bad_type.error().kind(), ErrorKind::kCorruptIndex);
}

TEST(FstArcTest, ExposesFlagsAndCopiesAllFields) {
    Arc arc = Arc::with_outputs(std::any{BytesRef()}, std::any{BytesRef()});
    arc.set_label('a');
    arc.set_target(77);
    arc.set_flags(static_cast<uint8_t>(BIT_FINAL_ARC | BIT_LAST_ARC | BIT_ARC_HAS_OUTPUT));
    arc.set_next_arc(99);
    arc.set_node_flags(ARCS_FOR_BINARY_SEARCH);
    arc.set_bytes_per_arc(3);
    arc.set_num_arcs(5);
    arc.set_first_label('a');
    arc.set_arc_idx(2);
    arc.set_pos_arcs_start(11);
    arc.set_presence_index(1);
    arc.set_next_final_output(std::any{BytesRef::from_string("final")});

    EXPECT_TRUE(arc.is_final());
    EXPECT_TRUE(arc.is_last());
    EXPECT_FALSE(arc.is_final() && !arc.has_output());
    EXPECT_TRUE(arc.has_output());
    EXPECT_FALSE(arc.target_is_next());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('a'));
    EXPECT_EQ(arc.target(), 77);
    EXPECT_EQ(arc.next_arc(), 99);
    EXPECT_EQ(arc.node_flags(), ARCS_FOR_BINARY_SEARCH);
    EXPECT_EQ(arc.bytes_per_arc(), 3);
    EXPECT_EQ(arc.num_arcs(), 5);
    EXPECT_EQ(arc.first_label(), static_cast<int32_t>('a'));
    EXPECT_EQ(arc.arc_idx(), 2);
    EXPECT_EQ(arc.pos_arcs_start(), 11);
    EXPECT_EQ(arc.presence_index(), 1);

    Arc copy;
    copy.copy_from(arc);
    EXPECT_EQ(copy.label(), arc.label());
    EXPECT_EQ(copy.target(), arc.target());
    EXPECT_EQ(copy.flags(), arc.flags());
    EXPECT_EQ(copy.node_flags(), arc.node_flags());
    EXPECT_EQ(copy.to_string().find("Arc("), 0u);

    // Constants match the Java/Lucene bit layout.
    EXPECT_EQ(ARCS_FOR_CONTINUOUS, static_cast<uint8_t>(ARCS_FOR_DIRECT_ADDRESSING | ARCS_FOR_BINARY_SEARCH));
    EXPECT_EQ(END_LABEL, -1);
    EXPECT_EQ(FINAL_END_NODE, -1);
    EXPECT_EQ(NON_FINAL_END_NODE, 0);
}

TEST(FstTest, FirstArcReflectsTheEmptyOutput) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    // No empty output: the start arc is only "last".
    FSTMetadata plain;
    plain.outputs = &outputs;
    plain.start_node = 7;
    FST plain_fst(plain, std::make_unique<BufferReader>(std::span<const uint8_t>()));
    Arc arc;
    plain_fst.get_first_arc(arc);
    EXPECT_EQ(arc.flags(), BIT_LAST_ARC);
    EXPECT_FALSE(arc.is_final());
    EXPECT_EQ(arc.target(), 7);
    EXPECT_TRUE(outputs.is_no_output(arc.next_final_output()));

    // With an empty output the start arc is also final and carries it.
    FSTMetadata with_empty;
    with_empty.outputs = &outputs;
    with_empty.start_node = 0;
    with_empty.empty_output = std::any{BytesRef::from_string("x")};
    FST fst(with_empty, std::make_unique<BufferReader>(std::span<const uint8_t>()));
    fst.get_first_arc(arc);
    EXPECT_TRUE(arc.is_final());
    EXPECT_TRUE(arc.is_last());
    EXPECT_TRUE(arc.has_final_output());
    EXPECT_EQ(arc.target(), 0);
    EXPECT_EQ(as_bytes(arc.next_final_output()).length(), 1u);
    EXPECT_FALSE(target_has_arcs(arc));

    // A target > 0 means the arc points at a real node.
    Arc pointing;
    pointing.set_target(3);
    EXPECT_TRUE(target_has_arcs(pointing));
}

TEST(FstTest, ReadEndArcOnlyAppliesToFinalArcs) {
    Arc follow;
    follow.set_flags(BIT_FINAL_ARC);
    follow.set_target(0);
    follow.set_next_final_output(std::any{BytesRef::from_string("final")});

    Arc arc;
    ASSERT_TRUE(read_end_arc(follow, arc));
    EXPECT_EQ(arc.label(), END_LABEL);
    EXPECT_EQ(arc.flags(), BIT_LAST_ARC);
    EXPECT_EQ(as_bytes(arc.output()).length(), 5u);

    // A final arc with a real target continues at that node.
    follow.set_flags(static_cast<uint8_t>(BIT_FINAL_ARC));
    follow.set_target(42);
    Arc continuing;
    ASSERT_TRUE(read_end_arc(follow, continuing));
    EXPECT_EQ(continuing.flags(), 0);
    EXPECT_EQ(continuing.next_arc(), 42);

    // A non-final arc has no END_LABEL arc.
    follow.set_flags(0);
    Arc untouched;
    EXPECT_FALSE(read_end_arc(follow, untouched));
}

TEST(FstTest, ReadsLabelsThroughTheMetadataInputType) {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    std::vector<uint8_t> buffer(16, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(write_label(out, InputType::kByte2, 40000).has_value());

    const auto written = std::span<const uint8_t>(buffer.data(), out.get_position());
    FSTMetadata metadata;
    metadata.outputs = &outputs;
    metadata.input_type = InputType::kByte2;
    FST fst(metadata, std::make_unique<BufferReader>(written));

    auto label = fst.read_label(fst.reader());
    ASSERT_TRUE(label.has_value()) << (label ? "" : label.error().to_string());
    EXPECT_EQ(label.value(), 40000);
    EXPECT_EQ(fst.outputs().type_name(), "ByteSequenceOutputs");
    EXPECT_EQ(fst.start_node(), 0);
    EXPECT_EQ(fst.num_bytes(), 0);
}

TEST(FstTest, WalksAVariableLengthNode) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    // A node with two variable-length arcs:
    //   [flags=0x00]['a'][target=3][flags=LAST|FINAL|STOP]['b']
    const std::vector<uint8_t> bytes{
        0x00,                          // arc 1: no flags
        static_cast<uint8_t>('a'),     // arc 1 label
        0x03,                          // arc 1 target == address of arc 2
        static_cast<uint8_t>(BIT_LAST_ARC | BIT_FINAL_ARC | BIT_STOP_NODE),
        static_cast<uint8_t>('b'),     // arc 2 label
    };

    FSTMetadata metadata;
    metadata.outputs = &outputs;
    metadata.start_node = 0;
    FST fst(metadata, std::make_unique<BufferReader>(bytes));

    Arc arc;
    auto first = fst.read_first_real_target_arc(0, arc, fst.reader());
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('a'));
    EXPECT_EQ(arc.target(), 3);
    EXPECT_EQ(arc.next_arc(), 3);
    EXPECT_FALSE(arc.is_last());
    EXPECT_TRUE(outputs.is_no_output(arc.output()));

    auto second = fst.read_next_real_arc(arc, fst.reader());
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('b'));
    EXPECT_TRUE(arc.is_last());
    EXPECT_TRUE(arc.is_final());
    EXPECT_TRUE(arc.is_stop_node());
    EXPECT_EQ(arc.target(), FINAL_END_NODE);
    EXPECT_EQ(arc.next_arc(), 5);

    // Reading past the last arc is a caller error; the reader hits the end.
    auto past_end = fst.read_next_arc(arc, fst.reader());
    EXPECT_FALSE(past_end.has_value());

    // A fake END_LABEL arc with no next node cannot be advanced either.
    Arc fake;
    fake.set_label(END_LABEL);
    fake.set_next_arc(0);
    auto illegal = fst.read_next_arc(fake, fst.reader());
    ASSERT_FALSE(illegal.has_value());
    EXPECT_EQ(illegal.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FstTest, FollowsAFinalArcIntoTheEndLabelArc) {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FSTMetadata metadata;
    metadata.outputs = &outputs;
    FST fst(metadata, std::make_unique<BufferReader>(std::span<const uint8_t>()));

    Arc follow;
    follow.set_flags(BIT_FINAL_ARC);
    follow.set_target(0);
    follow.set_next_final_output(std::any{BytesRef::from_string("f")});

    Arc arc;
    ASSERT_TRUE(fst.read_first_target_arc(follow, arc, fst.reader()).has_value());
    EXPECT_EQ(arc.label(), END_LABEL);
    EXPECT_TRUE(arc.is_final());
    EXPECT_TRUE(arc.is_last());
    EXPECT_EQ(arc.target(), FINAL_END_NODE);
    EXPECT_EQ(as_bytes(arc.output()).length(), 1u);

    // `readLastTargetArc` on a node without arcs yields the same virtual arc.
    Arc last;
    ASSERT_TRUE(fst.read_last_target_arc(follow, last, fst.reader()).has_value());
    EXPECT_EQ(last.label(), END_LABEL);
    EXPECT_TRUE(last.is_last());
    EXPECT_EQ(as_bytes(last.output()).length(), 1u);

    // A non-final follow arc without arcs is not expanded.
    Arc plain;
    plain.set_flags(0);
    plain.set_target(0);
    auto expanded = fst.is_expanded_target(plain, fst.reader());
    ASSERT_TRUE(expanded.has_value());
    EXPECT_FALSE(expanded.value());
}

TEST(FstTest, FindsArcsInAVariableLengthNode) {
    const auto &outputs = FstByteSequenceOutputs::singleton();

    // Node at address 1 (address 0 is a padding byte so that the node address is
    // representable as a "real" target). Three variable-length arcs:
    //   a -> no arcs, b -> no arcs, c -> last + stop
    const std::vector<uint8_t> bytes{
        0x00,                                          // padding
        0x00, static_cast<uint8_t>('a'), 0x00,         // arc a, target 0
        0x00, static_cast<uint8_t>('b'), 0x00,         // arc b, target 0
        static_cast<uint8_t>(BIT_LAST_ARC | BIT_STOP_NODE), static_cast<uint8_t>('c'),
    };

    FSTMetadata metadata;
    metadata.outputs = &outputs;
    FST fst(metadata, std::make_unique<BufferReader>(bytes));

    Arc arc;
    arc.set_target(1);
    ASSERT_TRUE(fst.find_target_arc(static_cast<int32_t>('b'), nullptr, arc, fst.reader()).value());
    EXPECT_EQ(arc.label(), static_cast<int32_t>('b'));

    // The first arc is reachable as well.
    Arc first;
    first.set_target(1);
    ASSERT_TRUE(fst.find_target_arc(static_cast<int32_t>('a'), nullptr, first, fst.reader()).value());
    EXPECT_EQ(first.label(), static_cast<int32_t>('a'));

    // The last arc uses STOP, so it has no target byte.
    Arc last;
    last.set_target(1);
    ASSERT_TRUE(fst.find_target_arc(static_cast<int32_t>('c'), nullptr, last, fst.reader()).value());
    EXPECT_EQ(last.label(), static_cast<int32_t>('c'));
    EXPECT_TRUE(last.is_last());

    // A label that is not present is reported as "not found".
    Arc missing;
    missing.set_target(1);
    auto not_found = fst.find_target_arc(static_cast<int32_t>('d'), nullptr, missing, fst.reader());
    ASSERT_TRUE(not_found.has_value());
    EXPECT_FALSE(not_found.value());
}

TEST(FstTest, FindsTheEndLabelOnlyForFinalArcs) {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FSTMetadata metadata;
    metadata.outputs = &outputs;
    FST fst(metadata, std::make_unique<BufferReader>(std::span<const uint8_t>()));

    Arc follow;
    follow.set_flags(BIT_FINAL_ARC);
    follow.set_target(0);
    follow.set_next_final_output(std::any{BytesRef::from_string("z")});

    Arc arc;
    auto found = fst.find_target_arc(END_LABEL, &follow, arc, fst.reader());
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found.value());
    EXPECT_EQ(arc.label(), END_LABEL);
    EXPECT_EQ(arc.flags(), BIT_LAST_ARC);
    EXPECT_EQ(as_bytes(arc.output()).length(), 1u);

    follow.set_flags(0);
    Arc not_final;
    auto absent = fst.find_target_arc(END_LABEL, &follow, not_final, fst.reader());
    ASSERT_TRUE(absent.has_value());
    EXPECT_FALSE(absent.value());
}
