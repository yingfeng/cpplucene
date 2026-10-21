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

/// Primary interface of the `clucene` module: a C++23 port of rlucene.
///
/// Subsystem partitions mirror the rlucene tree:
///   rlucene `core/util`        -> `clucene:bit_util`, `:bytes_ref`, ...
///   rlucene `core/store`       -> `clucene:data_input`, `:data_output`, ...
///   rlucene `core/document`    -> `clucene:document`, ...
///   rlucene `core/analysis`    -> `clucene:analyzer`, ...
///   rlucene `core/index`       -> `clucene:index_writer`, ...
///   rlucene `core/search`      -> `clucene:index_searcher`, ...
export module clucene;

// ---- util ----
export import :types;
export import :debug;
export import :error;
export import :bit_util;
export import :math_util;
export import :core_helper;
export import :array_util;
export import :constants;
export import :bytes_ref;
export import :bytes_ref_builder;
export import :string_helper;

// ---- store ----
export import :io_context;
export import :data_input;
export import :data_output;
