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

// Preprocessor helpers for the clucene port.
//
// Macros cannot be exported from a C++23 module, so they live in this plain
// header.  Consumers that want `CLUCENE_TRY` include it explicitly next to
// `import clucene;`.
#ifndef CLUCENE_MACROS_H
#define CLUCENE_MACROS_H

// Propagates an unexpected `LuceneError` from an expression returning
// `std::expected`.  The enclosing function must return a `std::expected` too.
//
//   CLUCENE_TRY(auto value, input->read_vint());
#define CLUCENE_TRY(decl, expr)                 \
    auto &&_clucene_r_##decl = (expr);          \
    if (!_clucene_r_##decl) {                   \
        return std::unexpected(_clucene_r_##decl.error()); \
    }                                           \
    decl = std::move(*_clucene_r_##decl)

// Discards the value and only propagates the error.
#define CLUCENE_TRY_VOID(expr)                  \
    do {                                        \
        auto &&_clucene_r = (expr);             \
        if (!_clucene_r) {                      \
            return std::unexpected(_clucene_r.error()); \
        }                                       \
    } while (false)

#endif // CLUCENE_MACROS_H
