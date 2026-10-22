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
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.freq_prox_terms_writer;

import lucene_core;

using namespace lucene;

namespace {

std::shared_ptr<FieldInfo> make_field(std::string name, IndexOptions options = IndexOptions::DocsAndFreqs) {
    auto field = std::make_shared<FieldInfo>();
    field->name = std::move(name);
    field->index_options = options;
    return field;
}

/// Adds one token to `per_field` for `doc_id`.
void add_token(FreqProxTermsWriterPerField &per_field, std::string_view text, int32_t doc_id,
               IndexContext &context, AttributeSource &attributes, int32_t position = 0, int32_t start = 0,
               int32_t end = 0) {
    FieldInvertState state(6, per_field.field_info()->name, per_field.field_info()->index_options);
    state.set_position(position);
    state.set_offset(0);
    attributes.get_attribute<OffsetAttribute>()->set_offset(static_cast<uint32_t>(start),
                                                           static_cast<uint32_t>(end));
    attributes.get_attribute<PayloadAttribute>()->set_payload(std::nullopt);
    auto bytes = BytesRef::from_string(text);
    ASSERT_TRUE(per_field.add_with_bytes_ref(&bytes, doc_id, state, attributes, context).has_value());
}

} // namespace

TEST(FreqProxTermsWriterTest, PrepareFieldsSortsByNameAndDropsEmptyFields) {
    FreqProxTermsWriter writer;
    IndexContext context;
    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    attributes.add_attribute<PayloadAttribute>();
    attributes.add_attribute<TermFrequencyAttribute>();

    std::vector<FreqProxTermsWriterPerField> fields;
    fields.push_back(std::move(writer.add_field(make_field("title")).value()));
    fields.push_back(std::move(writer.add_field(make_field("empty")).value()));
    fields.push_back(std::move(writer.add_field(make_field("body")).value()));

    add_token(fields[0], "hello", 0, context, attributes, 0, 0, 5);
    add_token(fields[2], "world", 0, context, attributes, 0, 0, 5);
    // `empty` never sees a token: it must not reach the postings consumer.

    auto prepared = writer.prepare_fields(std::move(fields), context.byte_pool);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
    ASSERT_EQ(prepared.value().size(), 2U);
    EXPECT_EQ(prepared.value()[0]->field_info()->name, "body");
    EXPECT_EQ(prepared.value()[1]->field_info()->name, "title");
    EXPECT_EQ(prepared.value()[0]->base().get_num_terms(), 1U);
    EXPECT_EQ(prepared.value()[1]->base().get_num_terms(), 1U);
}

TEST(FreqProxTermsWriterTest, PrepareFieldsWithNoTermsReturnsNothing) {
    FreqProxTermsWriter writer;
    IndexContext context;

    std::vector<FreqProxTermsWriterPerField> fields;
    fields.push_back(std::move(writer.add_field(make_field("a")).value()));
    fields.push_back(std::move(writer.add_field(make_field("b")).value()));

    auto prepared = writer.prepare_fields(std::move(fields), context.byte_pool);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_TRUE(prepared.value().empty());
}

TEST(FreqProxTermsWriterTest, TracksVectorFlagsAndDocumentLifecycle) {
    FreqProxTermsWriter writer;
    EXPECT_FALSE(writer.has_vectors());
    writer.set_has_vectors();
    EXPECT_TRUE(writer.has_vectors());

    EXPECT_TRUE(writer.start_document().has_value());
    IndexContext context;
    EXPECT_TRUE(writer.finish_document(0, context.freq_prox_term_int_pool, context.term_vectors_byte_pool,
                                       context.byte_pool)
                        .has_value());
}

TEST(FreqProxTermsWriterTest, AbortResetsThePools) {
    FreqProxTermsWriter writer;
    IndexContext context;
    ByteBlockPool next_byte_pool;
    IntBlockPool next_int_pool;

    ASSERT_TRUE(context.byte_pool.next_buffer().has_value());
    ASSERT_TRUE(context.freq_prox_term_int_pool.next_buffer().has_value());

    ASSERT_TRUE(writer.abort(context.freq_prox_term_int_pool, context.byte_pool, next_int_pool, next_byte_pool)
                        .has_value());
    EXPECT_EQ(context.byte_pool.byte_upto(), BYTE_BLOCK_SIZE);
    EXPECT_EQ(context.freq_prox_term_int_pool.int_upto(), INT_BLOCK_SIZE);
}
