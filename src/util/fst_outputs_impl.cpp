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

module lucene_core:fst_outputs.impl;

import :fst_outputs;
import :types;
import :error;
import :data_input;
import :data_output;

import std;

namespace lucene {

void FstOutputs::subtract_assign(FstOutput &output, const FstOutput &inc) const {
    output = subtract(output, inc);
}

void FstOutputs::add_assign(FstOutput &prefix, FstOutput &output) const {
    prefix = add(prefix, output);
}

VoidResult FstOutputs::write_final_output(const FstOutput &output, DataOutput &out) const {
    return write(output, out);
}

VoidResult FstOutputs::skip_output(DataInput &input) const {
    // Default: decode and discard.
    auto output = read(input);
    if (!output.has_value()) {
        return err(output.error());
    }
    return {};
}

Result<FstOutput> FstOutputs::read_final_output(DataInput &input) const {
    return read(input);
}

VoidResult FstOutputs::skip_final_output(DataInput &input) const {
    return skip_output(input);
}

Result<FstOutput> FstOutputs::merge(const FstOutput &first, const FstOutput &second) const {
    (void)first;
    (void)second;
    return err(LuceneError::unsupported_operation("merge is not supported by " + std::string(type_name())));
}

} // namespace lucene
