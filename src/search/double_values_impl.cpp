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

module lucene_core:double_values.impl;

import :double_values;
import :types;
import :error;

import std;

namespace lucene {

Result<double> WithDefaultDoubleValues::double_value() const {
    if (!has_value_) {
        return missing_value_;
    }
    return values_->double_value();
}

Result<bool> WithDefaultDoubleValues::advance_exact(int32_t doc) {
    auto advanced = values_->advance_exact(doc);
    if (!advanced) {
        return std::unexpected(std::move(advanced.error()));
    }
    has_value_ = advanced.value();
    // The wrapper always has a value: the default covers the missing ones.
    return true;
}

} // namespace lucene
