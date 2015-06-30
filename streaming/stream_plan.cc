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

#include "streaming/stream_plan.hh"

namespace streaming {

stream_plan& stream_plan::request_ranges(inet_address from, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges) {
    return request_ranges(from, connecting, keyspace, ranges, {});
}

stream_plan& stream_plan::request_ranges(inet_address from, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families) {
    auto& session = _coordinator.get_or_create_next_session(from, connecting);
    session.add_stream_request(keyspace, ranges, std::move(column_families), _repaired_at);
    return *this;
}

stream_plan& stream_plan::transfer_ranges(inet_address to, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families) {
    return transfer_ranges(to, to, keyspace, ranges, column_families);
}

stream_plan& stream_plan::transfer_ranges(inet_address to, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges) {
    return transfer_ranges(to, connecting, keyspace, ranges, {});
}

stream_plan& stream_plan::transfer_ranges(inet_address to, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families) {
    auto& session = _coordinator.get_or_create_next_session(to, connecting);
    session.add_transfer_ranges(keyspace, std::move(ranges), std::move(column_families), _flush_before_transfer, _repaired_at);
    return *this;
}

stream_plan& stream_plan::transfer_files(inet_address to, std::vector<stream_session::ss_table_streaming_sections> sstable_details) {
    _coordinator.transfer_files(to, std::move(sstable_details));
    return *this;
}

stream_plan& stream_plan::flush_before_transfer(bool flush_before_transfer_) {
    _flush_before_transfer = flush_before_transfer_;
    return *this;
}

}
