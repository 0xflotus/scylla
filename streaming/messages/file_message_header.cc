/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by Cloudius Systems.
 * Copyright 2015 Cloudius Systems.
 */
#include "streaming/messages/file_message_header.hh"
#include "types.hh"
#include "utils/serialization.hh"

namespace streaming {
namespace messages {

void file_message_header::serialize(bytes::iterator& out) const {
    cf_id.serialize(out);
    serialize_int32(out, sequence_number);
    serialize_string(out, version);
    serialize_int32(out, int32_t(format));
    serialize_int64(out, estimated_keys);
    serialize_int32(out, int32_t(sections.size()));
    for (auto& x : sections) {
        serialize_int64(out, x.first);
        serialize_int64(out, x.second);
    }
    comp_info.serialize(out);
    serialize_int64(out, repaired_at);
    serialize_int32(out, sstable_level);
}

file_message_header file_message_header::deserialize(bytes_view& v) {
    auto cf_id_ = UUID::deserialize(v);
    auto sequence_number_ = read_simple<int32_t>(v);
    auto version_ = read_simple_short_string(v);
    auto format_ = format_types(read_simple<int32_t>(v));
    auto estimated_keys_ = read_simple<int64_t>(v);
    auto num = read_simple<int32_t>(v);
    std::map<int64_t, int64_t> sections_;
    for (int32_t i = 0; i < num; i++) {
        auto key = read_simple<int64_t>(v);
        auto val = read_simple<int64_t>(v);
        sections_.emplace(key, val);
    }
    auto comp_info_ = compression_info::deserialize(v);
    auto repaired_at_ = read_simple<int64_t>(v);
    auto sstable_level_ = read_simple<int32_t>(v);

    return file_message_header(std::move(cf_id_), std::move(sequence_number_),
                               std::move(version_), format_, estimated_keys_,
                               std::move(sections_), std::move(comp_info_),
                               repaired_at_, sstable_level_);
}

size_t file_message_header::serialized_size() const {
    size_t size = cf_id.serialized_size();
    size += serialize_int32_size; //sequence_number
    size += serialize_string_size(version);
    size += serialize_int32_size; // format
    size += serialize_int64_size; // estimated_keys
    size += serialize_int32_size; // sections;
    size += (serialize_int64_size + serialize_int64_size) * sections.size();
    size += comp_info.serialized_size();
    size += serialize_int64_size;
    size += serialize_int32_size;
    return size;
}

} // namespace messages
} // namespace streaming
