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

module lucene_core:ut.term_state;

import lucene_core;

using namespace lucene;

TEST(OrdTermStateTest, CloneAndCopyFrom) {
    OrdTermState state{42};
    std::unique_ptr<TermState> cloned = state.clone();
    ASSERT_NE(cloned, nullptr);

    auto *cloned_ord = dynamic_cast<OrdTermState *>(cloned.get());
    ASSERT_NE(cloned_ord, nullptr);
    EXPECT_EQ(cloned_ord->ord, 42);

    OrdTermState target;
    ASSERT_TRUE(target.copy_from(*cloned).has_value());
    EXPECT_EQ(target.ord, 42);
    EXPECT_EQ(target.to_string(), "OrdTermState ord=42");
}

TEST(OrdTermStateTest, CopyFromRejectsForeignState) {
    OrdTermState target;

    // A different `TermState` implementation cannot be copied into an
    // `OrdTermState`.
    class OtherState : public TermState {
    public:
        [[nodiscard]] std::unique_ptr<TermState> clone() const override { return std::make_unique<OtherState>(); }
        [[nodiscard]] VoidResult copy_from(const TermState &) override { return {}; }
        [[nodiscard]] std::string to_string() const override { return "OtherState"; }
    };

    const OtherState other;
    auto result = target.copy_from(other);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
}
