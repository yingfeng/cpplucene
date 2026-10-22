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

module lucene_core:fst_no_outputs.impl;

import :fst_no_outputs;
import :fst_outputs;
import :types;
import :error;
import :data_input;
import :data_output;

import std;

namespace lucene {

const FstNoOutputs &FstNoOutputs::singleton() {
    static const FstNoOutputs instance;
    return instance;
}

std::any FstNoOutputs::common(const std::any & /*output1*/, const std::any & /*output2*/) const {
    return std::any{std::monostate{}};
}

std::any FstNoOutputs::subtract(const std::any &output, const std::any & /*inc*/) const {
    return output;
}

std::any FstNoOutputs::add(const std::any &prefix, const std::any & /*output*/) const {
    return prefix;
}

void FstNoOutputs::add_assign(std::any & /*prefix*/, std::any & /*output*/) const {
    // Nothing to combine: every value is NO_OUTPUT.
}

VoidResult FstNoOutputs::write(const std::any & /*output*/, DataOutput & /*out*/) const {
    return {};
}

Result<std::any> FstNoOutputs::read(DataInput & /*input*/) const {
    return std::any{std::monostate{}};
}

VoidResult FstNoOutputs::skip_output(DataInput & /*input*/) const {
    return {};
}

std::any FstNoOutputs::get_no_output() const {
    return std::any{std::monostate{}};
}

bool FstNoOutputs::is_no_output(const std::any & /*output*/) const {
    return true;
}

std::string FstNoOutputs::output_to_string(const std::any & /*output*/) const {
    return {};
}

Result<std::any> FstNoOutputs::merge(const std::any & /*first*/, const std::any & /*second*/) const {
    return std::any{std::monostate{}};
}

int64_t FstNoOutputs::ram_bytes_used(const std::any & /*output*/) const {
    return 0;
}

} // namespace lucene
