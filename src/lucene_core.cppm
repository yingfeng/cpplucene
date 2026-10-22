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

/// Primary interface of the `lucene_core` module: a C++23 port of rlucene.
///
/// Subsystem partitions mirror the rlucene tree:
///   rlucene `core/util`        -> `lucene_core:bit_util`, `:bytes_ref`, ...
///   rlucene `core/store`       -> `lucene_core:data_input`, `:data_output`, ...
///   rlucene `core/document`    -> `lucene_core:document`, ...
///   rlucene `core/analysis`    -> `lucene_core:analyzer`, ...
///   rlucene `core/index`       -> `lucene_core:index_writer`, ...
///   rlucene `core/search`      -> `lucene_core:index_searcher`, ...
export module lucene_core;

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
export import :crc32;
export import :number;
export import :lz4;
export import :lowercase_ascii_compression;
export import :numeric_utils;
export import :bits;
export import :bytes_ref_comparator;
export import :collection_util;
export import :packed_ints;
export import :direct_writer;
export import :direct_reader;
export import :direct_monotonic_writer;
export import :direct_monotonic_reader;
export import :packed64;
export import :growable_writer;
export import :fst_outputs;
export import :fst_no_outputs;
export import :fst_byte_sequence_outputs;
export import :fst_positive_int_outputs;
export import :fst_int_sequence_outputs;
export import :byte_block_pool;
export import :fst_bytes_reader;
export import :reverse_bytes_reader;
export import :growable_byte_array_data_output;
export import :ints_ref;
export import :read_write_data_output;
export import :bit_table_util;
export import :bkd_config;
export import :bkd_heap_point_writer;
export import :bkd_offline_point;
export import :bkd_radix_selector;
export import :bkd_point_writer;
export import :bkd_doc_ids_writer;
export import :bkd_tree_planner;
export import :bkd_leaf_writer;
export import :bkd_leaf_reader;
export import :bkd_index_tree;
export import :bkd_tree_reader;
export import :bkd_writer;
export import :bkd_util;
export import :fst;
export import :fst_builder;
export import :fst_store;
export import :fst_util;

// ---- codecs ----
export import :codec_util;

// ---- store ----
export import :io_context;
export import :data_input;
export import :data_output;
export import :random_access_input;
export import :index_input;
export import :index_output;
export import :lock;
export import :lock_factory;
export import :directory;
export import :byte_buffers_directory;
export import :fs_directory;

// ---- document ----
export import :index_types;
export import :field_type;
export import :indexable_field;
export import :document;
export import :field_kinds;

// ---- analysis ----
export import :attributes;
export import :attribute_source;
export import :token_stream;
export import :tokenizer;
export import :unicode_util;
export import :analyzer;
export import :lower_case_filter;
export import :stop_filter;
export import :simple_analyzers;
export import :standard_tokenizer;
export import :standard_analyzer;
