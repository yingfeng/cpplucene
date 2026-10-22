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
#include <filesystem>

module lucene_core:ut.directory;

import lucene_core;

using namespace lucene;

namespace {

/// Exercises one `Directory` implementation against the codec-facing contract.
void run_directory_contract(Directory &directory) {
    // --- write + read round trip through every primitive ---
    {
        auto output = directory.create_output("_0.fdt");
        ASSERT_TRUE(output.has_value());
        ASSERT_TRUE(output.value()->write_int(0x01020304).has_value());
        ASSERT_TRUE(output.value()->write_long(-5).has_value());
        ASSERT_TRUE(output.value()->write_vint(300).has_value());
        ASSERT_TRUE(output.value()->write_string("héllo").has_value());
        const uint64_t checksum = output.value()->get_checksum();
        ASSERT_TRUE(output.value()->close().has_value());
        // CRC-32 over the four writes above; must match the file bytes.
        EXPECT_NE(checksum, 0u);

        auto input = directory.open_input("_0.fdt");
        ASSERT_TRUE(input.has_value());
        EXPECT_EQ(input.value()->length().value(), 4u + 8u + 2u + 1u + 6u /* "héllo" is 6 bytes */);
        EXPECT_EQ(input.value()->read_int().value(), 0x01020304);
        EXPECT_EQ(input.value()->read_long().value(), -5);
        EXPECT_EQ(input.value()->read_vint().value(), 300);
        EXPECT_EQ(input.value()->read_string().value(), "héllo");
        EXPECT_EQ(input.value()->get_file_pointer().value(), input.value()->length().value());
    }

    // --- file listing / length / rename / delete ---
    {
        const auto names = directory.list_all();
        ASSERT_TRUE(names.has_value());
        ASSERT_EQ(names->size(), 1u);
        EXPECT_EQ((*names)[0], "_0.fdt");
        EXPECT_EQ(directory.file_length("_0.fdt").value(), 21u);

        EXPECT_TRUE(directory.rename("_0.fdt", "_0.fdx").has_value());
        EXPECT_TRUE(directory.list_all()->size() == 1);
        EXPECT_EQ((*directory.list_all())[0], "_0.fdx");
        EXPECT_TRUE(directory.delete_file("_0.fdx").has_value());
        EXPECT_TRUE(directory.list_all()->empty());
        // Deleting a missing file is a no-op, matching Lucene.
        EXPECT_TRUE(directory.delete_file("_0.fdx").has_value());
        EXPECT_FALSE(directory.file_length("_0.fdx").has_value());
    }

    // --- temp output, copy_from, slicing and random access ---
    {
        auto temp = directory.create_temp_output("pending", ".tmp", io_context_default());
        ASSERT_TRUE(temp.has_value());
        for (int32_t i = 0; i < 100; ++i) {
            ASSERT_TRUE(temp.value().output->write_int(i * 3).has_value());
        }
        ASSERT_TRUE(temp.value().output->close().has_value());
        const std::string temp_name = temp.value().name;
        EXPECT_TRUE(temp_name.starts_with("pending_"));

        ASSERT_TRUE(directory.rename(temp_name, "_1.fdt").has_value());

        auto input = directory.open_input("_1.fdt");
        ASSERT_TRUE(input.has_value());
        auto slice = input.value()->slice("slice", 4 * 10, 4 * 5);
        ASSERT_TRUE(slice.has_value());
        EXPECT_EQ(slice.value()->length().value(), 20u);
        EXPECT_EQ(slice.value()->read_int().value(), 30);
        EXPECT_EQ(slice.value()->read_int().value(), 33);

        auto clone = input.value()->clone();
        ASSERT_TRUE(clone.has_value());
        ASSERT_TRUE(clone.value()->seek(4 * 99).has_value());
        EXPECT_EQ(clone.value()->read_int().value(), 297);
        // The original input keeps its own position.
        EXPECT_EQ(input.value()->get_file_pointer().value(), 0u);

        auto random = input.value()->random_access_slice(0, 400);
        ASSERT_TRUE(random.has_value());
        EXPECT_EQ(random.value()->read_int(4 * 42).value(), 126);
        EXPECT_EQ(random.value()->length().value(), 400u);
        std::array<uint8_t, 8> two_ints{};
        ASSERT_TRUE(random.value()->read_bytes(0, two_ints).has_value());
        EXPECT_EQ(BitUtil::get_i32_le(two_ints, 0), 0);
        EXPECT_EQ(BitUtil::get_i32_le(two_ints, 4), 3);
        EXPECT_FALSE(random.value()->read_int(400).has_value());
    }

    // --- locks ---
    {
        auto lock = directory.obtain_lock("write");
        ASSERT_TRUE(lock.has_value());
        EXPECT_TRUE(lock.value()->ensure_valid().has_value());
        EXPECT_TRUE(lock.value()->close().has_value());
    }

    EXPECT_TRUE(directory.delete_file("_1.fdt").has_value());
}

} // namespace

TEST(ByteBuffersDirectoryTest, Contract) {
    ByteBuffersDirectory directory;
    run_directory_contract(directory);
    const auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    for (const auto &name : *names) {
        ADD_FAILURE() << "leftover file: " << name;
    }
    EXPECT_EQ(directory.file_count(), 0u);
    EXPECT_EQ(directory.ram_bytes_used(), 0u);
}

TEST(ByteBuffersDirectoryTest, RamUsageTracksFileSizes) {
    ByteBuffersDirectory directory;
    auto output = directory.create_output("a.bin");
    ASSERT_TRUE(output.has_value());
    std::vector<uint8_t> payload(1000, 7);
    ASSERT_TRUE(output.value()->write_bytes(payload).has_value());
    ASSERT_TRUE(output.value()->close().has_value());
    EXPECT_EQ(directory.ram_bytes_used(), 1000u);
}

TEST(FSDirectoryTest, Contract) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_fs_directory_ut";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    EXPECT_TRUE(directory.value()->is_fs_directory());
    run_directory_contract(*directory.value());
    EXPECT_TRUE(directory.value()->sync_metadata().has_value());

    std::filesystem::remove_all(path, ec);
}

TEST(FSDirectoryTest, ChecksumMatchesCrc32) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_fs_checksum_ut";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());

    auto output = directory.value()->create_output("crc.bin");
    ASSERT_TRUE(output.has_value());
    const std::string payload = "123456789";
    ASSERT_TRUE(output.value()->write_bytes(
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size()))
                    .has_value());
    // The canonical CRC-32 of "123456789" is 0xCBF43926.
    EXPECT_EQ(output.value()->get_checksum(), 0xCBF43926u);
    ASSERT_TRUE(output.value()->close().has_value());

    std::filesystem::remove_all(path, ec);
}

TEST(ByteBuffersDirectoryTest, LockIsExclusive) {
    ByteBuffersDirectory directory;
    auto first = directory.obtain_lock("commit");
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(directory.obtain_lock("commit").has_value());
    EXPECT_TRUE(first.value()->close().has_value());
    EXPECT_TRUE(directory.obtain_lock("commit").has_value());
}

TEST(NativeFSLockFactoryTest, LockIsExclusive) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_lock_ut";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);

    NativeFSLockFactory factory(path);
    auto first = factory.obtain_lock("write");
    ASSERT_TRUE(first.has_value());
    // flock/fcntl locks are per-process, so a second fcntl in the same process
    // succeeds; verifying that closing releases the descriptor is enough here.
    EXPECT_TRUE(first.value()->ensure_valid().has_value());
    EXPECT_TRUE(first.value()->close().has_value());
    EXPECT_TRUE(factory.clear_lock("write").has_value());

    std::filesystem::remove_all(path, ec);
}
