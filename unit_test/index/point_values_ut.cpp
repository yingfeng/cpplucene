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

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

module lucene_core:ut.point_values;

import lucene_core;

using namespace lucene;

namespace {

/// One leaf of the fake tree: the inclusive value range it covers and the
/// documents it holds.
struct Leaf {
    int32_t min_value{0};
    int32_t max_value{0};
    std::vector<int32_t> docs;
};

/// A one-dimensional KD-tree with a single level of leaves, which is enough to
/// exercise the `intersect` recursion of [`PointValues`].
class FakePointTree : public PointTree {
public:
    explicit FakePointTree(std::vector<Leaf> leaves) : leaves_(std::move(leaves)) {}

    [[nodiscard]] Result<bool> move_to_child() override {
        if (cursor_ != kRoot) {
            return false;
        }
        cursor_ = 0;
        return true;
    }

    [[nodiscard]] Result<bool> move_to_sibling() override {
        if (cursor_ == kRoot || cursor_ + 1 >= leaves_.size()) {
            return false;
        }
        ++cursor_;
        return true;
    }

    [[nodiscard]] Result<bool> move_to_parent() override {
        if (cursor_ == kRoot) {
            return false;
        }
        cursor_ = kRoot;
        return true;
    }

    [[nodiscard]] Result<BytesRef> get_min_packed_value() override { return packed(min_value()); }
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override { return packed(max_value()); }

    [[nodiscard]] Result<size_t> size() override {
        if (cursor_ == kRoot) {
            size_t total = 0;
            for (const auto &leaf : leaves_) {
                total += leaf.docs.size();
            }
            return total;
        }
        return leaves_[cursor_].docs.size();
    }

    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor &visitor) override {
        for (const auto &leaf : current_leaves()) {
            for (const int32_t doc : leaf.docs) {
                if (auto visited = visitor.visit(doc); !visited) {
                    return visited;
                }
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override {
        for (const auto &leaf : current_leaves()) {
            auto bytes = packed(leaf.min_value);
            if (!bytes) {
                return std::unexpected(std::move(bytes.error()));
            }
            for (const int32_t doc : leaf.docs) {
                if (auto visited = visitor.visit_with_packed_value(doc, bytes.value().as_slice()); !visited) {
                    return visited;
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<PointTree> try_clone() const override {
        return std::make_unique<FakePointTree>(leaves_);
    }

private:
    static constexpr size_t kRoot = std::numeric_limits<size_t>::max();
    static_assert(kRoot != 0);

    [[nodiscard]] int32_t min_value() const {
        if (cursor_ == kRoot) {
            int32_t min = leaves_.front().min_value;
            for (const auto &leaf : leaves_) {
                min = std::min(min, leaf.min_value);
            }
            return min;
        }
        return leaves_[cursor_].min_value;
    }

    [[nodiscard]] int32_t max_value() const {
        if (cursor_ == kRoot) {
            int32_t max = leaves_.front().max_value;
            for (const auto &leaf : leaves_) {
                max = std::max(max, leaf.max_value);
            }
            return max;
        }
        return leaves_[cursor_].max_value;
    }

    /// The leaves the cursor covers: the current leaf, or all of them at the
    /// root.
    [[nodiscard]] std::span<const Leaf> current_leaves() const {
        if (cursor_ == kRoot) {
            return leaves_;
        }
        return std::span<const Leaf>(leaves_).subspan(cursor_, 1);
    }

    /// Packed big-endian int32, which is what a one-dimensional BKD tree stores.
    static Result<BytesRef> packed(int32_t value) {
        std::vector<uint8_t> bytes(4);
        bytes[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        bytes[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        bytes[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        bytes[3] = static_cast<uint8_t>(value & 0xFF);
        return BytesRef::from_slice(std::move(bytes), 0, 4);
    }

    std::vector<Leaf> leaves_;
    size_t cursor_{kRoot};
};

int32_t unpack(std::span<const uint8_t> bytes) {
    return (static_cast<int32_t>(bytes[0]) << 24) | (static_cast<int32_t>(bytes[1]) << 16) |
           (static_cast<int32_t>(bytes[2]) << 8) | static_cast<int32_t>(bytes[3]);
}

/// Accepts documents whose value is in `[lower, upper]`.
class RangeVisitor : public IntersectVisitor {
public:
    RangeVisitor(int32_t lower, int32_t upper) : lower_(lower), upper_(upper) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        visited.push_back(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        const int32_t value = unpack(packed_value);
        if (value >= lower_ && value <= upper_) {
            visited.push_back(doc_id);
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        const int32_t min = unpack(min_packed_value);
        const int32_t max = unpack(max_packed_value);
        if (max < lower_ || min > upper_) {
            return Relation::kCellOutsideQuery;
        }
        if (min >= lower_ && max <= upper_) {
            return Relation::kCellInsideQuery;
        }
        return Relation::kCellCrossesQuery;
    }

    std::vector<int32_t> visited;

private:
    int32_t lower_{0};
    int32_t upper_{0};
};

std::vector<Leaf> four_leaves() {
    // Values 1..8 in four leaves of two documents each.
    return std::vector<Leaf>{
            Leaf{1, 2, {0, 1}},
            Leaf{3, 4, {2, 3}},
            Leaf{5, 6, {4, 5}},
            Leaf{7, 8, {6, 7}},
    };
}

/// A [`PointValues`] over the fake tree.
class FakePointValues : public PointValues {
public:
    explicit FakePointValues(std::vector<Leaf> leaves) : leaves_(std::move(leaves)) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override { return std::nullopt; }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override { return std::nullopt; }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return 4; }

    [[nodiscard]] Result<size_t> size() const override {
        size_t total = 0;
        for (const auto &leaf : leaves_) {
            total += leaf.docs.size();
        }
        return total;
    }

    [[nodiscard]] Result<int32_t> get_doc_count() const override { return static_cast<int32_t>(size().value()); }

    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(std::make_unique<FakePointTree>(leaves_));
    }

private:
    std::vector<Leaf> leaves_;
};

} // namespace

TEST(PointValuesTest, RelationOrdinals) {
    EXPECT_EQ(relation_ordinal(Relation::kCellInsideQuery), 0);
    EXPECT_EQ(relation_ordinal(Relation::kCellOutsideQuery), 1);
    EXPECT_EQ(relation_ordinal(Relation::kCellCrossesQuery), 2);
}

TEST(PointValuesTest, IntersectVisitsDocumentsInsideQuery) {
    FakePointValues values(four_leaves());

    // Values 3..6 are documents 2, 3, 4 and 5.
    RangeVisitor visitor(3, 6);
    ASSERT_TRUE(values.intersect(visitor).has_value());
    EXPECT_EQ(visitor.visited, (std::vector<int32_t>{2, 3, 4, 5}));
}

TEST(PointValuesTest, IntersectVisitsEverythingWhenQueryCoversTheRoot) {
    FakePointValues values(four_leaves());

    RangeVisitor visitor(0, 100);
    ASSERT_TRUE(values.intersect(visitor).has_value());
    EXPECT_EQ(visitor.visited.size(), 8u);
}

TEST(PointValuesTest, IntersectVisitsNothingWhenQueryMissesTheRoot) {
    FakePointValues values(four_leaves());

    RangeVisitor visitor(20, 30);
    ASSERT_TRUE(values.intersect(visitor).has_value());
    EXPECT_TRUE(visitor.visited.empty());
}

TEST(PointValuesTest, EstimatePointCountMatchesCellContents) {
    FakePointValues values(four_leaves());

    // Two whole leaves are inside, so the estimate is exact.
    const RangeVisitor inside(3, 6);
    auto estimate = values.estimate_point_count(inside);
    ASSERT_TRUE(estimate.has_value());
    EXPECT_EQ(estimate.value(), 4);

    // Nothing overlaps.
    const RangeVisitor outside(20, 30);
    auto none = values.estimate_point_count(outside);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value(), 0);
}

TEST(PointValuesTest, EstimateDocCountOfSingleValuedPoints) {
    FakePointValues values(four_leaves());

    const RangeVisitor visitor(3, 6);
    auto docs = values.estimate_doc_count(visitor);
    ASSERT_TRUE(docs.has_value());
    // size() == docCount(), so the point estimate is the document estimate.
    EXPECT_EQ(docs.value(), 4);
}

TEST(PointValuesTest, VisitorBulkMethodsDefaultToVisit) {
    RangeVisitor visitor(0, 100);
    const IntsRef refs(std::vector<int32_t>{4, 5, 6});
    ASSERT_TRUE(visitor.visit_with_ints_ref(refs).has_value());
    EXPECT_EQ(visitor.visited, (std::vector<int32_t>{4, 5, 6}));
}
