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

module clucene:io_context.impl;

import :io_context;

import :types;
import std;

namespace clucene {

Result<ReadAdvice> read_advice_from_str(std::string_view text) {
    std::string upper(text);
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper == "NORMAL") {
        return ReadAdvice::Normal;
    }
    if (upper == "RANDOM") {
        return ReadAdvice::Random;
    }
    if (upper == "SEQUENTIAL") {
        return ReadAdvice::Sequential;
    }
    if (upper == "RANDOM_PRELOAD") {
        return ReadAdvice::RandomPreload;
    }
    return err(LuceneError::illegal_argument(std::format("Invalid read advice: {}", text)));
}

Result<ReadAdvice> default_read_advice() {
    if (const char *value = std::getenv("apache.lucene.store.defaultReadAdvice"); value != nullptr) {
        auto parsed = read_advice_from_str(value);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        return parsed;
    }
    return ReadAdvice::Random;
}

Result<IOContext> IOContext::create(IOContextType context,
                                    ReadAdvice read_advice,
                                    std::optional<MergeInfo> merge_info,
                                    std::optional<FlushInfo> flush_info) {
    if (context == IOContextType::Merge && !merge_info) {
        return err(LuceneError::illegal_argument("merge_info must be provided if context is MERGE"));
    }
    if (context == IOContextType::Flush && !flush_info) {
        return err(LuceneError::illegal_argument("flush_info must be provided if context is FLUSH"));
    }
    if ((context == IOContextType::Flush || context == IOContextType::Merge) && read_advice != ReadAdvice::Sequential) {
        return err(LuceneError::illegal_argument(
            "The FLUSH and MERGE contexts must use the SEQUENTIAL read access advice"));
    }
    IOContext result;
    result.context_ = context;
    result.read_advice_ = read_advice;
    result.merge_info_ = std::move(merge_info);
    result.flush_info_ = std::move(flush_info);
    return result;
}

Result<IOContext> IOContext::with_read_advice(ReadAdvice read_advice) {
    return create(IOContextType::Default, read_advice, std::nullopt, std::nullopt);
}

Result<IOContext> IOContext::with_flush(FlushInfo flush_info) {
    return create(IOContextType::Flush, ReadAdvice::Sequential, std::nullopt, std::move(flush_info));
}

Result<IOContext> IOContext::with_merge(MergeInfo merge_info) {
    return create(IOContextType::Merge, ReadAdvice::Sequential, std::move(merge_info), std::nullopt);
}

Result<IOContext> IOContext::read_once_io_context() { return with_read_advice(ReadAdvice::Sequential); }

Result<IOContext> IOContext::with_read_advice_self(ReadAdvice read_advice) const {
    if (context_ == IOContextType::Default) {
        return with_read_advice(read_advice);
    }
    return *this;
}

const IOContext &io_context_default() {
    static const IOContext context = [] {
        auto advice = default_read_advice();
        auto built = IOContext::with_read_advice(advice.value_or(ReadAdvice::Random));
        return built.value_or(IOContext{});
    }();
    return context;
}

const IOContext &io_context_read_once() {
    static const IOContext context = [] {
        auto built = IOContext::read_once_io_context();
        return built.value_or(IOContext{});
    }();
    return context;
}

} // namespace clucene
