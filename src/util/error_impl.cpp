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

module clucene:error.impl;

import :error;

import :types;
import std;

namespace clucene {

std::string_view error_kind_name(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::kNone: return "none";
        case ErrorKind::kIo: return "io";
        case ErrorKind::kIllegalArgument: return "illegal argument";
        case ErrorKind::kIllegalState: return "illegal state";
        case ErrorKind::kArrayIndexOutOfBounds: return "array index out of bounds";
        case ErrorKind::kAlreadyClosed: return "already closed";
        case ErrorKind::kBufferAllocation: return "buffer allocation";
        case ErrorKind::kCollectionTerminated: return "collection terminated";
        case ErrorKind::kConcurrentModification: return "concurrent modification";
        case ErrorKind::kCorruptIndex: return "corrupt index";
        case ErrorKind::kEof: return "eof";
        case ErrorKind::kFuzzyTerms: return "fuzzy terms";
        case ErrorKind::kIndexFormatTooNew: return "index format too new";
        case ErrorKind::kIndexFormatTooOld: return "index format too old";
        case ErrorKind::kIndexNotFound: return "index not found";
        case ErrorKind::kLockAlreadyHeld: return "lock already held";
        case ErrorKind::kLockHeldByOther: return "lock held by other";
        case ErrorKind::kLockObtainFailed: return "lock obtain failed";
        case ErrorKind::kLockReleaseFailed: return "lock release failed";
        case ErrorKind::kMaxBytesLengthExceeded: return "max bytes length exceeded";
        case ErrorKind::kMergeAborted: return "merge aborted";
        case ErrorKind::kMerge: return "merge";
        case ErrorKind::kNeedImplemented: return "need implemented";
        case ErrorKind::kNoMoreTerms: return "no more terms";
        case ErrorKind::kNoSuchElement: return "no such element";
        case ErrorKind::kNotImplemented: return "not implemented";
        case ErrorKind::kNotSuchFile: return "no such file";
        case ErrorKind::kNumberFormat: return "number format";
        case ErrorKind::kNumberOverflow: return "number overflow";
        case ErrorKind::kTimeExceeded: return "time exceeded";
        case ErrorKind::kTooComplexToDeterminize: return "too complex to determinize";
        case ErrorKind::kTooManyClauses: return "too many clauses";
        case ErrorKind::kTooManyNestedClauses: return "too many nested clauses";
        case ErrorKind::kTragedy: return "tragedy";
        case ErrorKind::kUncheckedIo: return "unchecked io";
        case ErrorKind::kUnreachable: return "unreachable";
        case ErrorKind::kUnsupportedOperation: return "unsupported operation";
        case ErrorKind::kVersion: return "version";
        case ErrorKind::kParse: return "parse";
        case ErrorKind::kOther: return "other";
    }
    return "unknown";
}

LuceneError::LuceneError(ErrorKind kind, std::string message) : kind_(kind), message_(std::move(message)) {}

#define CLUCENE_DEFINE_ERROR_FACTORY(name, kind_value)                                                                                    \
    LuceneError LuceneError::name(std::string message) { return LuceneError(ErrorKind::kind_value, std::move(message)); }

CLUCENE_DEFINE_ERROR_FACTORY(io, kIo)
CLUCENE_DEFINE_ERROR_FACTORY(illegal_argument, kIllegalArgument)
CLUCENE_DEFINE_ERROR_FACTORY(illegal_state, kIllegalState)
CLUCENE_DEFINE_ERROR_FACTORY(array_index_out_of_bounds, kArrayIndexOutOfBounds)
CLUCENE_DEFINE_ERROR_FACTORY(already_closed, kAlreadyClosed)
CLUCENE_DEFINE_ERROR_FACTORY(buffer_allocation, kBufferAllocation)
CLUCENE_DEFINE_ERROR_FACTORY(collection_terminated, kCollectionTerminated)
CLUCENE_DEFINE_ERROR_FACTORY(concurrent_modification, kConcurrentModification)
CLUCENE_DEFINE_ERROR_FACTORY(corrupt_index, kCorruptIndex)
CLUCENE_DEFINE_ERROR_FACTORY(eof, kEof)
CLUCENE_DEFINE_ERROR_FACTORY(fuzzy_terms, kFuzzyTerms)
CLUCENE_DEFINE_ERROR_FACTORY(index_format_too_new, kIndexFormatTooNew)
CLUCENE_DEFINE_ERROR_FACTORY(index_format_too_old, kIndexFormatTooOld)
CLUCENE_DEFINE_ERROR_FACTORY(index_not_found, kIndexNotFound)
CLUCENE_DEFINE_ERROR_FACTORY(lock_already_held, kLockAlreadyHeld)
CLUCENE_DEFINE_ERROR_FACTORY(lock_held_by_other, kLockHeldByOther)
CLUCENE_DEFINE_ERROR_FACTORY(lock_obtain_failed, kLockObtainFailed)
CLUCENE_DEFINE_ERROR_FACTORY(lock_release_failed, kLockReleaseFailed)
CLUCENE_DEFINE_ERROR_FACTORY(max_bytes_length_exceeded, kMaxBytesLengthExceeded)
CLUCENE_DEFINE_ERROR_FACTORY(merge_aborted, kMergeAborted)
CLUCENE_DEFINE_ERROR_FACTORY(merge, kMerge)
CLUCENE_DEFINE_ERROR_FACTORY(need_implemented, kNeedImplemented)
CLUCENE_DEFINE_ERROR_FACTORY(no_more_terms, kNoMoreTerms)
CLUCENE_DEFINE_ERROR_FACTORY(no_such_element, kNoSuchElement)
CLUCENE_DEFINE_ERROR_FACTORY(not_implemented, kNotImplemented)
CLUCENE_DEFINE_ERROR_FACTORY(not_such_file, kNotSuchFile)
CLUCENE_DEFINE_ERROR_FACTORY(number_format, kNumberFormat)
CLUCENE_DEFINE_ERROR_FACTORY(number_overflow, kNumberOverflow)
CLUCENE_DEFINE_ERROR_FACTORY(time_exceeded, kTimeExceeded)
CLUCENE_DEFINE_ERROR_FACTORY(too_complex_to_determinize, kTooComplexToDeterminize)
CLUCENE_DEFINE_ERROR_FACTORY(too_many_clauses, kTooManyClauses)
CLUCENE_DEFINE_ERROR_FACTORY(too_many_nested_clauses, kTooManyNestedClauses)
CLUCENE_DEFINE_ERROR_FACTORY(tragedy, kTragedy)
CLUCENE_DEFINE_ERROR_FACTORY(unchecked_io, kUncheckedIo)
CLUCENE_DEFINE_ERROR_FACTORY(unreachable, kUnreachable)
CLUCENE_DEFINE_ERROR_FACTORY(unsupported_operation, kUnsupportedOperation)
CLUCENE_DEFINE_ERROR_FACTORY(version, kVersion)
CLUCENE_DEFINE_ERROR_FACTORY(parse, kParse)

#undef CLUCENE_DEFINE_ERROR_FACTORY

std::string LuceneError::to_string() const {
    std::string result;
    result.reserve(message_.size() + 32);
    result.append(error_kind_name(kind_));
    if (!message_.empty()) {
        result.append(": ").append(message_);
    }
    if (suppressed_) {
        result.append(" (suppressed: ").append(suppressed_->to_string()).append(")");
    }
    return result;
}

void LuceneError::add_suppressed(const LuceneError &error) {
    if (!suppressed_) {
        suppressed_ = std::make_shared<const LuceneError>(error);
        return;
    }
    // Chain rather than drop: keeps every suppressed failure reachable.
    auto tail = std::make_shared<LuceneError>(*suppressed_);
    tail->add_suppressed(error);
    suppressed_ = tail;
}

std::string to_string(const LuceneError &error) { return error.to_string(); }

} // namespace clucene
