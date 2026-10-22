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

// Preprocessor helpers for the lucene_core port.
//
// Macros cannot be exported from a C++23 module, so they live in this plain
// header.  Consumers that want `LUCENE_CORE_TRY` include it explicitly next to
// `import lucene_core;`.
#ifndef LUCENE_CORE_MACROS_H
#define LUCENE_CORE_MACROS_H

// Propagates an unexpected `LuceneError` from an expression returning
// `std::expected`.  The enclosing function must return a `std::expected` too.
//
//   LUCENE_CORE_TRY(auto value, input->read_vint());
#define LUCENE_CORE_TRY(decl, expr)                 \
    auto &&_lucene_core_r_##decl = (expr);          \
    if (!_lucene_core_r_##decl) {                   \
        return std::unexpected(_lucene_core_r_##decl.error()); \
    }                                           \
    decl = std::move(*_lucene_core_r_##decl)

// Discards the value and only propagates the error.
#define LUCENE_CORE_TRY_VOID(expr)                  \
    do {                                        \
        auto &&_lucene_core_r = (expr);             \
        if (!_lucene_core_r) {                      \
            return std::unexpected(_lucene_core_r.error()); \
        }                                       \
    } while (false)

#endif // LUCENE_CORE_MACROS_H
