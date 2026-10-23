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

module lucene_core:inet_address_point.impl;

import :inet_address_point;

import std;

import :error;
import :field_type;
import :indexable_field;
import :numeric_utils;
import :point_in_set_query;
import :point_range_query;
import :query;
import :types;

namespace lucene {

namespace {

/// `::ffff:0:0/96`, the prefix every IPv4-mapped address starts with.
constexpr std::array<uint8_t, 12> kIpv4Prefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

/// The `[value, value]` range query of a single point.
[[nodiscard]] Result<std::shared_ptr<Query>> range_query(std::string field, std::vector<uint8_t> lower_point,
                                                        std::vector<uint8_t> upper_point) {
    auto query = PointRangeQuery::create(std::move(field), std::move(lower_point), std::move(upper_point), 1);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointRangeQuery>(std::move(query.value())));
}

/// Parses a dotted-quad IPv4 address into `out[12..16]`.
[[nodiscard]] VoidResult parse_ipv4(std::string_view text, std::span<uint8_t> out) {
    size_t start = 0;
    for (size_t part = 0; part < 4; ++part) {
        const size_t dot = text.find('.', start);
        const bool last = part == 3;
        if (last != (dot == std::string_view::npos)) {
            return err(LuceneError::illegal_argument(std::format("invalid IPv4 address '{}'", text)));
        }
        const std::string_view digits = text.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                                                        : dot - start);
        if (digits.empty() || digits.size() > 3) {
            return err(LuceneError::illegal_argument(std::format("invalid IPv4 address '{}'", text)));
        }
        if (digits.size() > 1 && digits.front() == '0') {
            return err(LuceneError::illegal_argument(std::format("invalid IPv4 address '{}': leading zero", text)));
        }
        uint32_t value = 0;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') {
                return err(LuceneError::illegal_argument(std::format("invalid IPv4 address '{}'", text)));
            }
            value = value * 10 + static_cast<uint32_t>(digit - '0');
        }
        if (value > 0xff) {
            return err(LuceneError::illegal_argument(std::format("invalid IPv4 address '{}': octet out of range", text)));
        }
        out[12 + part] = static_cast<uint8_t>(value);
        start = dot + 1;
    }
    return {};
}

/// Parses one colon-separated group: either four hex digits or, in the last
/// position, an embedded IPv4 address (which yields two groups).
[[nodiscard]] VoidResult parse_groups(std::string_view text, std::vector<uint16_t> &groups, bool last_section) {
    if (text.empty()) {
        return {};
    }
    size_t start = 0;
    while (true) {
        const size_t colon = text.find(':', start);
        const std::string_view part = text.substr(start, colon == std::string_view::npos ? std::string_view::npos
                                                                                       : colon - start);
        if (part.empty()) {
            return err(LuceneError::illegal_argument(std::format("invalid IPv6 address: empty group in '{}'", text)));
        }
        if (part.find('.') != std::string_view::npos) {
            const bool is_last = colon == std::string_view::npos && last_section;
            if (!is_last) {
                return err(
                        LuceneError::illegal_argument(std::format("invalid IPv6 address: embedded IPv4 not last in '{}'",
                                                                  text)));
            }
            std::array<uint8_t, 16> mapped{};
            if (auto parsed = parse_ipv4(part, mapped); !parsed.has_value()) {
                return std::unexpected(std::move(parsed.error()));
            }
            groups.push_back(static_cast<uint16_t>((mapped[12] << 8) | mapped[13]));
            groups.push_back(static_cast<uint16_t>((mapped[14] << 8) | mapped[15]));
        } else {
            if (part.size() > 4) {
                return err(LuceneError::illegal_argument(std::format("invalid IPv6 address: group too long in '{}'",
                                                                    text)));
            }
            uint16_t value = 0;
            for (const char digit : part) {
                uint16_t nibble = 0;
                if (digit >= '0' && digit <= '9') {
                    nibble = static_cast<uint16_t>(digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                    nibble = static_cast<uint16_t>(digit - 'a' + 10);
                } else if (digit >= 'A' && digit <= 'F') {
                    nibble = static_cast<uint16_t>(digit - 'A' + 10);
                } else {
                    return err(LuceneError::illegal_argument(std::format("invalid IPv6 address: bad digit in '{}'",
                                                                        text)));
                }
                value = static_cast<uint16_t>((value << 4) | nibble);
            }
            groups.push_back(value);
        }
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    return {};
}

} // namespace

bool InetAddressPoint::is_ipv4_mapped(const Address &address) noexcept {
    return std::equal(kIpv4Prefix.begin(), kIpv4Prefix.end(), address.begin());
}

Result<Field> InetAddressPoint::create(std::string name, const Address &point) {
    FieldType type;
    if (auto set = type.set_dimensions(1, BYTES); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    type.freeze();
    return Field::from_bytes(std::move(name), std::vector<uint8_t>(point.begin(), point.end()), std::move(type));
}

Result<InetAddressPoint::Address> InetAddressPoint::next_up(const Address &address) {
    if (address == MAX_VALUE) {
        return err(LuceneError::number_overflow(
                std::format("Overflow: there is no greater InetAddress than {}", to_string(address))));
    }
    std::array<uint8_t, BYTES> delta{};
    delta[BYTES - 1] = 1;
    Address next{};
    if (auto added = NumericUtils::add(BYTES, 0, address, delta, next); !added.has_value()) {
        return std::unexpected(std::move(added.error()));
    }
    return next;
}

Result<InetAddressPoint::Address> InetAddressPoint::next_down(const Address &address) {
    if (address == MIN_VALUE) {
        return err(LuceneError::number_overflow(
                std::format("Underflow: there is no smaller InetAddress than {}", to_string(address))));
    }
    std::array<uint8_t, BYTES> delta{};
    delta[BYTES - 1] = 1;
    Address next{};
    if (auto subtracted = NumericUtils::subtract(BYTES, 0, address, delta, next); !subtracted.has_value()) {
        return std::unexpected(std::move(subtracted.error()));
    }
    return next;
}

Result<InetAddressPoint::Address> InetAddressPoint::parse(std::string_view text) {
    Address address{};
    if (text.find(':') == std::string_view::npos) {
        std::copy(kIpv4Prefix.begin(), kIpv4Prefix.end(), address.begin());
        if (auto parsed = parse_ipv4(text, address); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        return address;
    }

    const size_t compress = text.find("::");
    if (compress != std::string_view::npos && text.find("::", compress + 2) != std::string_view::npos) {
        return err(LuceneError::illegal_argument(std::format("invalid IPv6 address '{}': multiple '::'", text)));
    }
    std::vector<uint16_t> head;
    std::vector<uint16_t> tail;
    if (compress == std::string_view::npos) {
        if (auto parsed = parse_groups(text, head, true); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (head.size() != 8) {
            return err(LuceneError::illegal_argument(
                    std::format("invalid IPv6 address '{}': expected 8 groups, got {}", text, head.size())));
        }
    } else {
        if (auto parsed = parse_groups(text.substr(0, compress), head, false); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (auto parsed = parse_groups(text.substr(compress + 2), tail, true); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        // The compressed run has to stand for at least one zero group.
        if (head.size() + tail.size() > 7) {
            return err(LuceneError::illegal_argument(
                    std::format("invalid IPv6 address '{}': too many groups around '::'", text)));
        }
    }

    std::array<uint16_t, 8> groups{};
    std::copy(head.begin(), head.end(), groups.begin());
    size_t tail_index = groups.size();
    for (auto value = tail.rbegin(); value != tail.rend(); ++value) {
        groups[--tail_index] = *value;
    }
    for (size_t i = 0; i < 8; ++i) {
        address[2 * i] = static_cast<uint8_t>(groups[i] >> 8);
        address[2 * i + 1] = static_cast<uint8_t>(groups[i] & 0xff);
    }
    return address;
}

std::string InetAddressPoint::to_string(const Address &address) {
    if (is_ipv4_mapped(address)) {
        return std::format("{}.{}.{}.{}", address[12], address[13], address[14], address[15]);
    }
    std::string text;
    for (size_t i = 0; i < 8; ++i) {
        if (i != 0) {
            text.push_back(':');
        }
        text += std::format("{:x}", (static_cast<uint16_t>(address[2 * i]) << 8) | address[2 * i + 1]);
    }
    return text;
}

Result<std::shared_ptr<Query>> InetAddressPoint::new_exact_query(std::string field, const Address &value) {
    return new_range_query(std::move(field), value, value);
}

Result<std::shared_ptr<Query>> InetAddressPoint::new_prefix_query(std::string field, const Address &value,
                                                                  size_t prefix_length) {
    // Mask the *host* representation, so an IPv4 prefix of at most 32 bits covers
    // the last four bytes of the mapped form rather than the whole 128 bits.
    const bool mapped = is_ipv4_mapped(value);
    const size_t first_byte = mapped ? 12 : 0;
    const size_t host_bytes = BYTES - first_byte;
    if (prefix_length > 8 * host_bytes) {
        return err(LuceneError::illegal_argument(
                std::format("illegal prefixLength '{}'. Must be 0-{}", prefix_length, 8 * host_bytes)));
    }

    Address lower = value;
    Address upper = value;
    for (size_t i = prefix_length; i < 8 * host_bytes; ++i) {
        const uint8_t mask = static_cast<uint8_t>(1U << (7 - (i & 7)));
        lower[first_byte + (i >> 3)] &= static_cast<uint8_t>(~mask);
        upper[first_byte + (i >> 3)] |= mask;
    }
    return new_range_query(std::move(field), lower, upper);
}

Result<std::shared_ptr<Query>> InetAddressPoint::new_range_query(std::string field, const Address &lower_value,
                                                                 const Address &upper_value) {
    return range_query(std::move(field), std::vector<uint8_t>(lower_value.begin(), lower_value.end()),
                       std::vector<uint8_t>(upper_value.begin(), upper_value.end()));
}

Result<std::shared_ptr<Query>> InetAddressPoint::new_set_query(std::string field, std::span<const Address> values) {
    if (values.empty()) {
        return err(LuceneError::illegal_argument("values must contain at least 1 value"));
    }
    std::vector<std::vector<uint8_t>> points;
    points.reserve(values.size());
    for (const Address &value : values) {
        points.emplace_back(value.begin(), value.end());
    }
    auto query = PointInSetQuery::create(std::move(field), std::move(points), 1);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointInSetQuery>(std::move(query.value())));
}

} // namespace lucene
