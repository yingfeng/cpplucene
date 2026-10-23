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

export module lucene_core:inet_address_point;

import :types;
import std;

import :error;
import :indexable_field;
import :query;

namespace lucene {

/// An indexed 128-bit IP address field.
///
/// Port of `InetAddressPoint` (`core/document/inet_address_point.rs`).
///
/// IPv4 addresses are stored as IPv4-mapped IPv6 addresses (`1.2.3.4` is the
/// same point as `::ffff:1.2.3.4`), so one field handles both families and the
/// prefix compression of the point index still works.
///
/// **Deviation:** the reference implementation takes `std::net::IpAddr`, which
/// the port's standard library does not offer; addresses are therefore passed
/// around as their 16-byte encoding, with [`parse`] / [`to_string`] converting
/// to and from text.  The query factories return the generic [`PointRangeQuery`]
/// / [`PointInSetQuery`], whose `to_string` renders hexadecimal endpoints instead
/// of host addresses (the port's point queries have no formatter hook).
export struct InetAddressPoint {
    /// The number of bytes per dimension: 128 bits.
    static constexpr size_t BYTES = 16;

    /// The encoding of an address: always the 16-byte (IPv6 / mapped) form.
    using Address = std::array<uint8_t, BYTES>;

    /// The minimum value an address can hold (`::`).
    static constexpr Address MIN_VALUE{};
    /// The maximum value an address can hold (`ffff:...:ffff`).
    static constexpr Address MAX_VALUE{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    /// Whether `address` is in the IPv4-mapped range (`::ffff:0:0/96`).
    [[nodiscard]] static bool is_ipv4_mapped(const Address &address) noexcept;

    /// Indexes `point`.
    [[nodiscard]] static Result<Field> create(std::string name, const Address &point);

    /// The address that compares immediately after `address`.
    [[nodiscard]] static Result<Address> next_up(const Address &address);

    /// The address that compares immediately before `address`.
    [[nodiscard]] static Result<Address> next_down(const Address &address);

    /// Parses `text` as `a.b.c.d` or as a colon-separated IPv6 address.
    ///
    /// IPv4 input is stored in the mapped form; an IPv6 input that already is in
    /// the mapped range is stored as is (the two are byte-identical).
    [[nodiscard]] static Result<Address> parse(std::string_view text);

    /// Renders the host address: `a.b.c.d` for mapped addresses, otherwise the
    /// eight hexadecimal groups joined by `:` (the reference implementation does
    /// not compress the zero run).
    [[nodiscard]] static std::string to_string(const Address &address);

    // ---- queries ----

    /// Matches the documents whose point is exactly `value`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, const Address &value);

    /// Matches the documents whose point is inside the CIDR network described by
    /// `value` and `prefix_length` (0-32 for IPv4, 0-128 for IPv6).
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_prefix_query(std::string field, const Address &value,
                                                                        size_t prefix_length);

    /// Matches the documents whose point lies in `[lower_value, upper_value]`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_range_query(std::string field, const Address &lower_value,
                                                                       const Address &upper_value);

    /// Matches the documents whose point is one of `values`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_set_query(std::string field,
                                                                      std::span<const Address> values);
};

} // namespace lucene
