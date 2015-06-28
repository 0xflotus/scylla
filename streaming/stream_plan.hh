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

#pragma once

#include "utils/UUID.hh"
#include "utils/UUID_gen.hh"
#include "core/sstring.hh"
#include "gms/inet_address.hh"
#include "query-request.hh"
#include "dht/i_partitioner.hh"
#include "streaming/stream_coordinator.hh"
#include <vector>

namespace streaming {

/**
 * {@link StreamPlan} is a helper class that builds StreamOperation of given configuration.
 *
 * This is the class you want to use for building streaming plan and starting streaming.
 */
class stream_plan {
private:
    using inet_address = gms::inet_address;
    using UUID = utils::UUID;
    using token = dht::token;
    UUID _plan_id;
    sstring _description;
    //List<StreamEventHandler> handlers = new ArrayList<>();
    long _repaired_at;
    stream_coordinator _coordinator;

    bool _flush_before_transfer = true;
    // FIXME: ActiveRepairService.UNREPAIRED_SSTABLE
    long UNREPAIRED_SSTABLE = 0;
public:

    /**
     * Start building stream plan.
     *
     * @param description Stream type that describes this StreamPlan
     */
    stream_plan(sstring description)
        : stream_plan(description, UNREPAIRED_SSTABLE, 1, false) {
    }

    stream_plan(sstring description, bool keep_ss_table_levels)
        : stream_plan(description, UNREPAIRED_SSTABLE, 1, keep_ss_table_levels) {
    }

    stream_plan(sstring description, long repaired_at, int connections_per_host, bool keep_ss_table_levels)
        : _plan_id(utils::UUID_gen::get_time_UUID())
        , _description(description)
        , _repaired_at(repaired_at)
        , _coordinator(connections_per_host, keep_ss_table_levels) {
    }

    /**
     * Request data in {@code keyspace} and {@code ranges} from specific node.
     *
     * @param from endpoint address to fetch data from.
     * @param connecting Actual connecting address for the endpoint
     * @param keyspace name of keyspace
     * @param ranges ranges to fetch
     * @return this object for chaining
     */
    stream_plan& request_ranges(inet_address from, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges);

    /**
     * Request data in {@code columnFamilies} under {@code keyspace} and {@code ranges} from specific node.
     *
     * @param from endpoint address to fetch data from.
     * @param connecting Actual connecting address for the endpoint
     * @param keyspace name of keyspace
     * @param ranges ranges to fetch
     * @param columnFamilies specific column families
     * @return this object for chaining
     */
    stream_plan& request_ranges(inet_address from, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families);

    /**
     * Add transfer task to send data of specific {@code columnFamilies} under {@code keyspace} and {@code ranges}.
     *
     * @see #transferRanges(java.net.InetAddress, java.net.InetAddress, String, java.util.Collection, String...)
     */
    stream_plan& transfer_ranges(inet_address to, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families);

    /**
     * Add transfer task to send data of specific keyspace and ranges.
     *
     * @param to endpoint address of receiver
     * @param connecting Actual connecting address of the endpoint
     * @param keyspace name of keyspace
     * @param ranges ranges to send
     * @return this object for chaining
     */
    stream_plan& transfer_ranges(inet_address to, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges);

    /**
     * Add transfer task to send data of specific {@code columnFamilies} under {@code keyspace} and {@code ranges}.
     *
     * @param to endpoint address of receiver
     * @param connecting Actual connecting address of the endpoint
     * @param keyspace name of keyspace
     * @param ranges ranges to send
     * @param columnFamilies specific column families
     * @return this object for chaining
     */
    stream_plan& transfer_ranges(inet_address to, inet_address connecting, sstring keyspace, std::vector<query::range<token>> ranges, std::vector<sstring> column_families);

    /**
     * Add transfer task to send given SSTable files.
     *
     * @param to endpoint address of receiver
     * @param sstableDetails sstables with file positions and estimated key count.
     *                       this collection will be modified to remove those files that are successfully handed off
     * @return this object for chaining
     */
    stream_plan& transfer_files(inet_address to, std::vector<stream_session::ss_table_streaming_sections> sstable_details);
#if 0

    public StreamPlan listeners(StreamEventHandler handler, StreamEventHandler... handlers)
    {
        this.handlers.add(handler);
        if (handlers != null)
            Collections.addAll(this.handlers, handlers);
        return this;
    }

    /**
     * Set custom StreamConnectionFactory to be used for establishing connection
     *
     * @param factory StreamConnectionFactory to use
     * @return self
     */
    public StreamPlan connectionFactory(StreamConnectionFactory factory)
    {
        this.coordinator.setConnectionFactory(factory);
        return this;
    }
#endif
public:
    /**
     * @return true if this plan has no plan to execute
     */
    bool is_empty() {
        return !_coordinator.has_active_sessions();
    }
#if 0

    /**
     * Execute this {@link StreamPlan} asynchronously.
     *
     * @return Future {@link StreamState} that you can use to listen on progress of streaming.
     */
    public StreamResultFuture execute()
    {
        return StreamResultFuture.init(planId, description, handlers, coordinator);
    }

    /**
     * Set flushBeforeTransfer option.
     * When it's true, will flush before streaming ranges. (Default: true)
     *
     * @param flushBeforeTransfer set to true when the node should flush before transfer
     * @return this object for chaining
     */
    public StreamPlan flushBeforeTransfer(boolean flushBeforeTransfer)
    {
        this.flushBeforeTransfer = flushBeforeTransfer;
        return this;
    }
#endif
};

} // namespace streaming
