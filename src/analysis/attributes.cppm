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

export module lucene_core:attributes;

import :types;

import std;

import :bytes_ref;

namespace lucene {

/// Base class of every token attribute.  Port of `Attribute` / `AttributeImpl`.
///
/// The reference implementation drives attributes through a typed enum plus `RefCell` guards; here the
/// Java design is used instead (one polymorphic implementation per attribute,
/// looked up by type in an [`AttributeSource`]), which maps 1:1 onto the same
/// observable behaviour without the borrow gymnastics.
/// The address of a per-type tag, used by [`AttributeSource`] to look an
/// attribute up by its concrete type.
///
/// The lookup runs once per attribute per token, and a `dynamic_cast` per
/// candidate dominated an indexing run (it compares type names through
/// `strcmp`).  Comparing these addresses is a pointer compare, and each type has
/// exactly one tag for the whole program.
template <class T>
[[nodiscard]] const void *attribute_type_tag() noexcept {
    static const char tag = 0;
    return &tag;
}

export class Attribute {
public:
    virtual ~Attribute() = default;

    /// The address of this attribute's [`attribute_type_tag`].
    [[nodiscard]] virtual const void *type_tag() const noexcept = 0;

    /// A deep copy with the same state.
    [[nodiscard]] virtual std::unique_ptr<Attribute> clone() const = 0;

    /// Resets every field to its default.
    virtual void clear() = 0;

    /// Copies the state of `other` (which must have this attribute's concrete
    /// type) into this instance, keeping the object identity intact.
    ///
    /// This is what [`AttributeSource::restore_state`] uses; a `clone()`-based
    /// restore would replace the instances and invalidate every pointer a
    /// filter cached in its constructor.
    virtual void copy_from(const Attribute &other) { (void)other; }

    /// The attribute name used in debug output.
    [[nodiscard]] virtual std::string_view name() const = 0;
};

/// The token text.  Port of `CharTermAttribute`.
export class CharTermAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<CharTermAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "CharTermAttribute"; }

    [[nodiscard]] std::string_view text() const noexcept { return std::string_view(buffer_.data(), length_); }
    [[nodiscard]] std::string to_string() const { return std::string(text()); }
    [[nodiscard]] const char *buffer() const noexcept { return buffer_.data(); }
    /// The text buffer, writable up to [`length`].
    ///
    /// Filters that rewrite a token in place (`LowerCaseFilter` for ASCII) use
    /// this to avoid building a second string per token.
    [[nodiscard]] char *mutable_buffer() noexcept { return buffer_.data(); }
    [[nodiscard]] size_t length() const noexcept { return length_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
    [[nodiscard]] char32_t char_at(size_t index) const;

    void set_length(size_t length) noexcept { length_ = length; }
    void set_empty() noexcept { length_ = 0; }
    void set_text(std::string_view text);
    void append(std::string_view text);
    void append(char32_t codepoint);
    /// Ensures the backing buffer holds at least `capacity` bytes.
    void resize_buffer(size_t capacity);
    /// Returns the text as UTF-32 code points (used by the tokenizers).
    [[nodiscard]] std::u32string to_u32string() const;

    [[nodiscard]] std::string_view sub_sequence(size_t start, size_t end) const;

private:
    std::vector<char> buffer_{std::vector<char>(16, '\0')};
    size_t length_{0};
};

/// Byte-level term, used by `TermToBytesRefAttribute` implementations.
export class BytesTermAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<BytesTermAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "BytesTermAttribute"; }

    void set_bytes(std::span<const uint8_t> bytes);
    [[nodiscard]] std::span<const uint8_t> bytes_ref() const noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_{};
};

/// `[start_offset, end_offset)` of the token in the original text.
export class OffsetAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<OffsetAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "OffsetAttribute"; }

    [[nodiscard]] uint32_t start_offset() const noexcept { return start_offset_; }
    [[nodiscard]] uint32_t end_offset() const noexcept { return end_offset_; }
    void set_offset(uint32_t start_offset, uint32_t end_offset) noexcept {
        start_offset_ = start_offset;
        end_offset_ = end_offset;
    }

private:
    uint32_t start_offset_{0};
    uint32_t end_offset_{0};
};

/// Difference between the token's position and the previous token's.
export class PositionIncrementAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<PositionIncrementAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "PositionIncrementAttribute"; }

    [[nodiscard]] uint32_t position_increment() const noexcept { return position_increment_; }
    void set_position_increment(uint32_t value) noexcept { position_increment_ = value; }

private:
    uint32_t position_increment_{1};
};

/// Number of positions spanned by the token (default 1).
export class PositionLengthAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<PositionLengthAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "PositionLengthAttribute"; }

    [[nodiscard]] uint32_t position_length() const noexcept { return position_length_; }
    void set_position_length(uint32_t value) noexcept { position_length_ = value; }

private:
    uint32_t position_length_{1};
};

/// Lexical type of the token, e.g. `<ALPHANUM>`.
export class TypeAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<TypeAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "TypeAttribute"; }

    [[nodiscard]] std::string_view type() const noexcept { return type_; }
    void set_type(std::string_view type) { type_ = std::string(type); }

private:
    std::string type_{"word"};
};

/// Marks a token as a keyword (searchable verbatim, not stemmed).
export class KeywordAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<KeywordAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "KeywordAttribute"; }

    [[nodiscard]] bool is_keyword() const noexcept { return keyword_; }
    void set_keyword(bool value) noexcept { keyword_ = value; }

private:
    bool keyword_{false};
};

/// Optional per-token payload.
export class PayloadAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<PayloadAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "PayloadAttribute"; }

    [[nodiscard]] const std::optional<BytesRef> &payload() const noexcept { return payload_; }
    void set_payload(std::optional<BytesRef> payload) noexcept { payload_ = std::move(payload); }

private:
    std::optional<BytesRef> payload_{};
};

/// Scaled term frequency (used by `DelimitedTermFrequencyTokenFilter`).
export class TermFrequencyAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<TermFrequencyAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "TermFrequencyAttribute"; }

    [[nodiscard]] uint32_t term_frequency() const noexcept { return term_frequency_; }
    void set_term_frequency(uint32_t value) noexcept { term_frequency_ = value; }

private:
    uint32_t term_frequency_{1};
};

/// Arbitrary per-token flags (`FlagsAttribute`).
export class FlagsAttribute : public Attribute {
public:
    [[nodiscard]] const void *type_tag() const noexcept override {
        return attribute_type_tag<FlagsAttribute>();
    }
    [[nodiscard]] std::unique_ptr<Attribute> clone() const override;
    void copy_from(const Attribute &other) override;
    void clear() override;
    [[nodiscard]] std::string_view name() const override { return "FlagsAttribute"; }

    [[nodiscard]] int32_t flags() const noexcept { return flags_; }
    void set_flags(int32_t value) noexcept { flags_ = value; }

private:
    int32_t flags_{0};
};

} // namespace lucene
