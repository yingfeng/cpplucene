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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.inet_address;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "ip";

using Address = InetAddressPoint::Address;

Address parse_ok(std::string_view text) {
    auto address = InetAddressPoint::parse(text);
    EXPECT_TRUE(address.has_value()) << (address ? "" : address.error().to_string());
    return address.has_value() ? address.value() : Address{};
}

std::vector<int32_t> search_docs(const Query &query, std::shared_ptr<DirectoryReader> &reader) {
    std::vector<int32_t> docs;
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    return docs;
}

/// Indexes one address point per entry and reopens the reader.
std::shared_ptr<DirectoryReader> index_points(const std::vector<std::string_view> &addresses) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const std::string_view text : addresses) {
        auto field = InetAddressPoint::create(std::string(kField), parse_ok(text));
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        Document doc;
        doc.add(std::make_unique<Field>(std::move(field.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value());
    return reader.has_value() ? reader.value() : nullptr;
}

/// Indexes one address range per entry and reopens the reader.
std::shared_ptr<DirectoryReader> index_ranges(const std::vector<std::pair<std::string_view, std::string_view>> &ranges) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const auto &[min, max] : ranges) {
        auto field = InetAddressRange::create(std::string(kField), parse_ok(min), parse_ok(max));
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        Document doc;
        doc.add(std::make_unique<InetAddressRange>(std::move(field.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(InetAddressTest, ParsesAndRendersIpv4AndIpv6) {
    const Address v4 = parse_ok("192.168.0.1");
    EXPECT_TRUE(InetAddressPoint::is_ipv4_mapped(v4));
    EXPECT_EQ(v4[10], 0xff);
    EXPECT_EQ(v4[11], 0xff);
    EXPECT_EQ(v4[12], 192);
    EXPECT_EQ(v4[15], 1);
    EXPECT_EQ(InetAddressPoint::to_string(v4), "192.168.0.1");

    // The mapped form and the plain IPv6 spelling are the same point.
    EXPECT_EQ(parse_ok("::ffff:192.168.0.1"), v4);
    EXPECT_EQ(parse_ok("0:0:0:0:0:ffff:c0a8:1"), v4);

    const Address v6 = parse_ok("2001:db8::1");
    EXPECT_FALSE(InetAddressPoint::is_ipv4_mapped(v6));
    EXPECT_EQ(v6[0], 0x20);
    EXPECT_EQ(v6[1], 0x01);
    EXPECT_EQ(v6[2], 0x0d);
    EXPECT_EQ(v6[3], 0xb8);
    EXPECT_EQ(v6[15], 0x01);
    EXPECT_EQ(InetAddressPoint::to_string(v6), "2001:db8:0:0:0:0:0:1");
    EXPECT_EQ(parse_ok(InetAddressPoint::to_string(v6)), v6);

    EXPECT_EQ(parse_ok("::"), InetAddressPoint::MIN_VALUE);
    EXPECT_EQ(parse_ok("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), InetAddressPoint::MAX_VALUE);
}

TEST(InetAddressTest, RejectsMalformedAddresses) {
    for (const std::string_view text : {"", "1.2.3", "1.2.3.4.5", "1.2.3.256", "01.2.3.4", "a.b.c.d", "1:2:3",
                                        "1::2::3", "1:2:3:4:5:6:7:8:9", "gggg::", "1:2:3:4:5:6:7::8",
                                        "::1.2.3.256", "1.2.3.4::"}) {
        const auto parsed = InetAddressPoint::parse(text);
        EXPECT_FALSE(parsed.has_value()) << "accepted '" << text << "'";
        if (!parsed.has_value()) {
            EXPECT_EQ(parsed.error().kind(), ErrorKind::kIllegalArgument) << text;
        }
    }
}

TEST(InetAddressTest, StepsToTheNeighbouringAddresses) {
    const Address one = parse_ok("10.0.0.1");
    const Address next = InetAddressPoint::next_up(one).value();
    EXPECT_EQ(InetAddressPoint::to_string(next), "10.0.0.2");
    EXPECT_EQ(InetAddressPoint::next_down(next).value(), one);

    // Crossing an octet boundary.
    const Address edge = parse_ok("10.0.0.255");
    EXPECT_EQ(InetAddressPoint::to_string(InetAddressPoint::next_up(edge).value()), "10.0.1.0");

    auto overflow = InetAddressPoint::next_up(InetAddressPoint::MAX_VALUE);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().kind(), ErrorKind::kNumberOverflow);
    auto underflow = InetAddressPoint::next_down(InetAddressPoint::MIN_VALUE);
    ASSERT_FALSE(underflow.has_value());
    EXPECT_EQ(underflow.error().kind(), ErrorKind::kNumberOverflow);
}

TEST(InetAddressTest, CreatesAPointField) {
    auto field = InetAddressPoint::create(std::string(kField), parse_ok("127.0.0.1"));
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().name(), kField);
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 1U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), InetAddressPoint::BYTES);
    auto bytes = field.value().binary_value();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value().size(), InetAddressPoint::BYTES);

    // A fresh value can be written into the same field.
    ASSERT_TRUE(field.value().set_bytes_value(std::vector<uint8_t>(InetAddressPoint::MIN_VALUE.begin(),
                                                                   InetAddressPoint::MIN_VALUE.end()))
                        .has_value());
    EXPECT_EQ(field.value().binary_value().value(), std::vector<uint8_t>(InetAddressPoint::BYTES, 0));
}

TEST(InetAddressTest, QueriesPointsByRangeSetAndPrefix) {
    auto reader = index_points({"1.2.3.4", "1.2.3.5", "1.2.4.1", "2.0.0.1"});
    ASSERT_NE(reader, nullptr);

    auto exact = InetAddressPoint::new_exact_query(std::string(kField), parse_ok("1.2.3.5"));
    ASSERT_TRUE(exact.has_value()) << (exact ? "" : exact.error().to_string());
    EXPECT_EQ(search_docs(*exact.value(), reader), std::vector<int32_t>({1}));

    auto range = InetAddressPoint::new_range_query(std::string(kField), parse_ok("1.2.3.0"), parse_ok("1.2.3.255"));
    ASSERT_TRUE(range.has_value()) << (range ? "" : range.error().to_string());
    EXPECT_EQ(search_docs(*range.value(), reader), std::vector<int32_t>({0, 1}));

    const std::array<Address, 2> values{parse_ok("1.2.4.1"), parse_ok("2.0.0.1")};
    auto set = InetAddressPoint::new_set_query(std::string(kField), values);
    ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
    EXPECT_EQ(search_docs(*set.value(), reader), std::vector<int32_t>({2, 3}));
}

TEST(InetAddressTest, QueriesPointsByCidrPrefix) {
    auto reader = index_points({"1.2.3.4", "1.2.3.5", "1.2.4.1", "2001:db8::1", "2001:db9::1"});
    ASSERT_NE(reader, nullptr);

    auto prefix = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("1.2.3.4"), 24);
    ASSERT_TRUE(prefix.has_value()) << (prefix ? "" : prefix.error().to_string());
    EXPECT_EQ(search_docs(*prefix.value(), reader), std::vector<int32_t>({0, 1}));

    // A single host prefix.
    auto host = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("1.2.3.4"), 32);
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(search_docs(*host.value(), reader), std::vector<int32_t>({0}));

    auto network = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("2001:db8::1"), 32);
    ASSERT_TRUE(network.has_value()) << (network ? "" : network.error().to_string());
    EXPECT_EQ(search_docs(*network.value(), reader), std::vector<int32_t>({3}));

    auto too_long = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("1.2.3.4"), 33);
    ASSERT_FALSE(too_long.has_value());
    EXPECT_EQ(too_long.error().kind(), ErrorKind::kIllegalArgument);

    auto ipv6_too_long = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("2001:db8::1"), 129);
    ASSERT_FALSE(ipv6_too_long.has_value());
}

TEST(InetAddressTest, EncodesAndDecodesRanges) {
    auto field = InetAddressRange::create(std::string(kField), parse_ok("10.0.0.1"), parse_ok("10.0.0.9"));
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().name(), kField);
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), InetAddressRange::BYTES);
    auto bytes = field.value().binary_value();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value().size(), 2 * InetAddressRange::BYTES);

    EXPECT_EQ(InetAddressPoint::to_string(field.value().get_min().value()), "10.0.0.1");
    EXPECT_EQ(InetAddressPoint::to_string(field.value().get_max().value()), "10.0.0.9");
    EXPECT_EQ(field.value().to_string(), "InetAddressRange <ip: [10.0.0.1 : 10.0.0.9]>");
    EXPECT_FALSE(field.value().numeric_value().has_value());

    auto replaced = field.value().set_range_values(parse_ok("192.168.1.0"), parse_ok("192.168.1.255"));
    ASSERT_TRUE(replaced.has_value()) << (replaced ? "" : replaced.error().to_string());
    EXPECT_EQ(InetAddressPoint::to_string(field.value().get_min().value()), "192.168.1.0");

    auto inverted = InetAddressRange::create(std::string(kField), parse_ok("10.0.0.9"), parse_ok("10.0.0.1"));
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(InetAddressTest, QueriesTheRelationOfTheIndexedRanges) {
    // Documents: [10.0.0.1, 10.0.0.5], [10.0.0.3, 10.0.0.20], [10.0.1.0, 10.0.1.10].
    auto reader = index_ranges({{"10.0.0.1", "10.0.0.5"}, {"10.0.0.3", "10.0.0.20"}, {"10.0.1.0", "10.0.1.10"}});
    ASSERT_NE(reader, nullptr);

    auto intersects = InetAddressRange::new_intersects_query(std::string(kField), parse_ok("10.0.0.4"),
                                                             parse_ok("10.0.0.4"));
    ASSERT_TRUE(intersects.has_value()) << (intersects ? "" : intersects.error().to_string());
    EXPECT_EQ(search_docs(intersects.value(), reader), std::vector<int32_t>({0, 1}));

    auto contains = InetAddressRange::new_contains_query(std::string(kField), parse_ok("10.0.0.4"),
                                                         parse_ok("10.0.0.5"));
    ASSERT_TRUE(contains.has_value()) << (contains ? "" : contains.error().to_string());
    EXPECT_EQ(search_docs(contains.value(), reader), std::vector<int32_t>({0, 1}));

    auto within = InetAddressRange::new_within_query(std::string(kField), parse_ok("10.0.0.0"),
                                                     parse_ok("10.0.1.255"));
    ASSERT_TRUE(within.has_value()) << (within ? "" : within.error().to_string());
    EXPECT_EQ(search_docs(within.value(), reader), std::vector<int32_t>({0, 1, 2}));

    // Crosses: neither disjoint nor contained — [10.0.0.1, 10.0.0.5] starts before
    // the query range and [10.0.0.3, 10.0.0.20] ends after it, while the third
    // document is disjoint.
    auto crosses = InetAddressRange::new_crosses_query(std::string(kField), parse_ok("10.0.0.2"),
                                                       parse_ok("10.0.0.10"));
    ASSERT_TRUE(crosses.has_value()) << (crosses ? "" : crosses.error().to_string());
    EXPECT_EQ(search_docs(crosses.value(), reader), std::vector<int32_t>({0, 1}));
}

TEST(InetAddressTest, DescribesTheRangeQueries) {
    auto query = InetAddressRange::new_intersects_query(std::string(kField), parse_ok("10.0.0.1"),
                                                        parse_ok("10.0.0.9"));
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_NE(query.value().to_string({}).value().find("[10.0.0.1 : 10.0.0.9]"), std::string::npos);
}

TEST(InetAddressTest, DescribesThePrefixQueryInHex) {
    // The port's point queries have no host-address formatter, so the endpoints
    // are printed as hexadecimal bytes; check that the masked range is right.
    auto query = InetAddressPoint::new_prefix_query(std::string(kField), parse_ok("1.2.3.4"), 24);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    const auto *range = dynamic_cast<const PointRangeQuery *>(query.value().get());
    ASSERT_NE(range, nullptr);
    std::vector<uint8_t> expected_lower(InetAddressPoint::BYTES, 0);
    expected_lower[10] = 0xff;
    expected_lower[11] = 0xff;
    expected_lower[12] = 1;
    expected_lower[13] = 2;
    expected_lower[14] = 3;
    std::vector<uint8_t> expected_upper = expected_lower;
    expected_upper[15] = 0xff;
    EXPECT_EQ(range->lower_point(), expected_lower);
    EXPECT_EQ(range->upper_point(), expected_upper);
}
