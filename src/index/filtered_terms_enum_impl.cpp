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

module lucene_core:filtered_terms_enum.impl;

import :filtered_terms_enum;

import :attribute_source;
import :block_term_state;
import :bytes_ref;
import :error;
import :impacts_enum;
import :postings_enum;
import :terms_enum;
import :types;

import std;

namespace lucene {

Result<std::optional<BytesRef>> FilteredTermsEnumBase::next_seek_term(const BytesRef * /*current*/) {
    return err(LuceneError::need_implemented("unfiltered terms enum has no next seek term"));
}

FilteredTermsEnum::FilteredTermsEnum(std::unique_ptr<TermsEnum> tenum, std::unique_ptr<FilteredTermsEnumBase> hook,
                                     bool do_seek)
    : do_seek_(do_seek), tenum_(std::move(tenum)), hook_(std::move(hook)) {}

std::unique_ptr<FilteredTermsEnum> FilteredTermsEnum::create(std::unique_ptr<TermsEnum> tenum,
                                                            std::unique_ptr<FilteredTermsEnumBase> hook,
                                                            bool start_with_seek) {
    return std::unique_ptr<FilteredTermsEnum>(
            new FilteredTermsEnum(std::move(tenum), std::move(hook), start_with_seek));
}

std::unique_ptr<FilteredTermsEnum> FilteredTermsEnum::unfiltered(std::unique_ptr<TermsEnum> tenum) {
    return std::unique_ptr<FilteredTermsEnum>(new FilteredTermsEnum(std::move(tenum), nullptr, false));
}

Result<std::optional<BytesRef>> FilteredTermsEnum::hook_next_seek_term(const BytesRef *current) {
    auto next = hook_->next_seek_term(current);
    if (next) {
        return next;
    }
    if (next.error().kind() == ErrorKind::kNeedImplemented || next.error().kind() == ErrorKind::kNotImplemented) {
        // rlucene: a hook that does not implement `next_seek_term` starts from
        // the initial seek term, once.
        auto initial = std::move(initial_seek_term_);
        initial_seek_term_.reset();
        return std::optional<BytesRef>(std::move(initial));
    }
    return std::unexpected(std::move(next.error()));
}

Result<const BytesRef *> FilteredTermsEnum::next() {
    if (!hook_) {
        return tenum_->next();
    }

    while (true) {
        if (do_seek_) {
            do_seek_ = false;

            const BytesRef *current = nullptr;
            if (has_actual_term_) {
                auto term = tenum_->term();
                if (!term) {
                    return std::unexpected(std::move(term.error()));
                }
                current = term.value();
            }

            auto seek_term = hook_next_seek_term(current);
            if (!seek_term) {
                return std::unexpected(std::move(seek_term.error()));
            }
            if (!seek_term->has_value()) {
                has_actual_term_ = false;
                return static_cast<const BytesRef *>(nullptr);
            }

            auto status = tenum_->seek_ceil((*seek_term)->as_slice());
            if (!status) {
                return std::unexpected(std::move(status.error()));
            }
            if (status.value() == SeekStatus::End) {
                has_actual_term_ = false;
                return static_cast<const BytesRef *>(nullptr);
            }
            has_actual_term_ = true;
        } else {
            auto advanced = tenum_->next();
            if (!advanced) {
                return std::unexpected(std::move(advanced.error()));
            }
            if (advanced.value() == nullptr) {
                has_actual_term_ = false;
                return static_cast<const BytesRef *>(nullptr);
            }
            has_actual_term_ = true;
        }

        int64_t ord = 0;
        if (hook_->need_ord()) {
            auto term_ord = tenum_->ord();
            if (!term_ord) {
                return std::unexpected(std::move(term_ord.error()));
            }
            ord = term_ord.value();
        }

        auto term = tenum_->term();
        if (!term) {
            return std::unexpected(std::move(term.error()));
        }
        auto accepted = hook_->accept(*term.value(), ord);
        if (!accepted) {
            return std::unexpected(std::move(accepted.error()));
        }

        switch (accepted.value()) {
            case AcceptStatus::kYesAndSeek:
                // term accepted, but we need to seek next time
                do_seek_ = true;
                return term.value();
            case AcceptStatus::kYes:
                return term.value();
            case AcceptStatus::kNoAndSeek:
                // invalid term, seek next time
                do_seek_ = true;
                break;
            case AcceptStatus::kEnd:
                // we are supposed to end the enum
                return static_cast<const BytesRef *>(nullptr);
            case AcceptStatus::kNo:
                // we just iterate again
                break;
        }
    }
}

Result<AttributeSource *> FilteredTermsEnum::attributes() {
    return tenum_->attributes();
}

Result<AttributeSource *> FilteredTermsEnum::attributes_mut() {
    return tenum_->attributes_mut();
}

Result<bool> FilteredTermsEnum::seek_exact(std::span<const uint8_t> term) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum::seek_exact"));
    }
    return tenum_->seek_exact(term);
}

Result<bool> FilteredTermsEnum::prepare_seek_exact(std::span<const uint8_t> term) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum does not support seeking"));
    }
    return tenum_->prepare_seek_exact(term);
}

Result<bool> FilteredTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum does not support seeking"));
    }
    return tenum_->get_prepare_seek_exact_status(target);
}

Result<SeekStatus> FilteredTermsEnum::seek_ceil(std::span<const uint8_t> term) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum::seek_ceil"));
    }
    return tenum_->seek_ceil(term);
}

VoidResult FilteredTermsEnum::seek_exact_with_ord(int64_t ord) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum::seek_exact_with_ord"));
    }
    return tenum_->seek_exact_with_ord(ord);
}

VoidResult FilteredTermsEnum::seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum &state) {
    if (hook_) {
        return err(LuceneError::unsupported_operation("FilteredTermsEnum::seek_exact_with_state"));
    }
    return tenum_->seek_exact_with_state(term, state);
}

Result<const BytesRef *> FilteredTermsEnum::term() const {
    return tenum_->term();
}

Result<int64_t> FilteredTermsEnum::ord() const {
    return tenum_->ord();
}

Result<int32_t> FilteredTermsEnum::doc_freq() {
    return tenum_->doc_freq();
}

Result<int64_t> FilteredTermsEnum::total_term_freq() {
    return tenum_->total_term_freq();
}

Result<std::unique_ptr<PostingsEnum>> FilteredTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                            int32_t flags) {
    return tenum_->postings_with_flags(std::move(reuse), flags);
}

Result<std::unique_ptr<ImpactsEnum>> FilteredTermsEnum::impacts(int32_t flags) {
    return tenum_->impacts(flags);
}

Result<TermStateEnum> FilteredTermsEnum::term_state() {
    return tenum_->term_state();
}

} // namespace lucene
