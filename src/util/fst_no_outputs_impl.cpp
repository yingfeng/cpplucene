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

FstOutput FstNoOutputs::common(const FstOutput & /*output1*/, const FstOutput & /*output2*/) const {
    return FstOutput{std::monostate{}};
}

FstOutput FstNoOutputs::subtract(const FstOutput &output, const FstOutput & /*inc*/) const {
    return output;
}

FstOutput FstNoOutputs::add(const FstOutput &prefix, const FstOutput & /*output*/) const {
    return prefix;
}

void FstNoOutputs::add_assign(FstOutput & /*prefix*/, FstOutput & /*output*/) const {
    // Nothing to combine: every value is NO_OUTPUT.
}

VoidResult FstNoOutputs::write(const FstOutput & /*output*/, DataOutput & /*out*/) const {
    return {};
}

Result<FstOutput> FstNoOutputs::read(DataInput & /*input*/) const {
    return FstOutput{std::monostate{}};
}

VoidResult FstNoOutputs::skip_output(DataInput & /*input*/) const {
    return {};
}

FstOutput FstNoOutputs::get_no_output() const {
    return FstOutput{std::monostate{}};
}

bool FstNoOutputs::is_no_output(const FstOutput & /*output*/) const {
    return true;
}

std::string FstNoOutputs::output_to_string(const FstOutput & /*output*/) const {
    return {};
}

Result<FstOutput> FstNoOutputs::merge(const FstOutput & /*first*/, const FstOutput & /*second*/) const {
    return FstOutput{std::monostate{}};
}

int64_t FstNoOutputs::ram_bytes_used(const FstOutput & /*output*/) const {
    return 0;
}

} // namespace lucene
