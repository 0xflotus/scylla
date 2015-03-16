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
 */

/*
 * Copyright 2014 Cloudius Systems
 *
 * Modified by Cloudius Systems
 */

#ifndef CQL3_CQL_QUERY_OPTIONS_HH
#define CQL3_CQL_QUERY_OPTIONS_HH

#include "database.hh"
#include "db/consistency_level.hh"
#include "service/query_state.hh"
#include "service/pager/paging_state.hh"
#include "cql3/column_specification.hh"
#include "cql3/column_identifier.hh"
#include "serialization_format.hh"

namespace cql3 {

class default_query_options;

/**
 * Options for a query.
 */
class query_options {
    serialization_format _serialization_format;
public:
    explicit query_options(serialization_format sf) : _serialization_format(sf) {}
    // Options that are likely to not be present in most queries
    struct specific_options final {
        static const specific_options DEFAULT;

        const int32_t page_size;
        const ::shared_ptr<service::pager::paging_state> state;
        const std::experimental::optional<db::consistency_level> serial_consistency;
        const api::timestamp_type timestamp;
    };

    // It can't be const because of prepare()
    static default_query_options DEFAULT;

    virtual ~query_options() {}

#if 0
    public static final CBCodec<QueryOptions> codec = new Codec();

    public static QueryOptions fromProtocolV1(ConsistencyLevel consistency, List<ByteBuffer> values)
    {
        return new DefaultQueryOptions(consistency, values, false, SpecificOptions.DEFAULT, 1);
    }

    public static QueryOptions fromProtocolV2(ConsistencyLevel consistency, List<ByteBuffer> values)
    {
        return new DefaultQueryOptions(consistency, values, false, SpecificOptions.DEFAULT, 2);
    }

    public static QueryOptions forInternalCalls(ConsistencyLevel consistency, List<ByteBuffer> values)
    {
        return new DefaultQueryOptions(consistency, values, false, SpecificOptions.DEFAULT, 3);
    }

    public static QueryOptions forInternalCalls(List<ByteBuffer> values)
    {
        return new DefaultQueryOptions(ConsistencyLevel.ONE, values, false, SpecificOptions.DEFAULT, 3);
    }

    public static QueryOptions fromPreV3Batch(ConsistencyLevel consistency)
    {
        return new DefaultQueryOptions(consistency, Collections.<ByteBuffer>emptyList(), false, SpecificOptions.DEFAULT, 2);
    }

    public static QueryOptions create(ConsistencyLevel consistency, List<ByteBuffer> values, boolean skipMetadata, int pageSize, PagingState pagingState, ConsistencyLevel serialConsistency)
    {
        return new DefaultQueryOptions(consistency, values, skipMetadata, new SpecificOptions(pageSize, pagingState, serialConsistency, -1L), 0);
    }
#endif

    virtual db::consistency_level get_consistency() const = 0;
    virtual const std::vector<bytes_opt>& get_values() const = 0;
    virtual bool skip_metadata() const = 0;

    /**  The pageSize for this query. Will be <= 0 if not relevant for the query.  */
    int32_t get_page_size() const { return get_specific_options().page_size; }

    /** The paging state for this query, or null if not relevant. */
    ::shared_ptr<service::pager::paging_state> get_paging_state() const {
        return get_specific_options().state;
    }

    /**  Serial consistency for conditional updates. */
    std::experimental::optional<db::consistency_level> get_serial_consistency() const {
        return get_specific_options().serial_consistency;
    }

    api::timestamp_type get_timestamp(service::query_state& state) const {
        auto tstamp = get_specific_options().timestamp;
        return tstamp != api::missing_timestamp ? tstamp : state.get_timestamp();
    }

    /**
     * The protocol version for the query. Will be 3 if the object don't come from
     * a native protocol request (i.e. it's been allocated locally or by CQL-over-thrift).
     */
    virtual int get_protocol_version() const = 0;
    serialization_format get_serialization_format() const { return _serialization_format; }

    // Mainly for the sake of BatchQueryOptions
    virtual const specific_options& get_specific_options() const = 0;

    virtual void prepare(const std::vector<::shared_ptr<column_specification>>& specs) {
    }

#if 0
    private static class Codec implements CBCodec<QueryOptions>
    {

        public void encode(QueryOptions options, ByteBuf dest, int version)
        {
            assert version >= 2;

            CBUtil.writeConsistencyLevel(options.getConsistency(), dest);

            EnumSet<Flag> flags = gatherFlags(options);
            dest.writeByte((byte)Flag.serialize(flags));

            if (flags.contains(Flag.VALUES))
                CBUtil.writeValueList(options.getValues(), dest);
            if (flags.contains(Flag.PAGE_SIZE))
                dest.writeInt(options.getPageSize());
            if (flags.contains(Flag.PAGING_STATE))
                CBUtil.writeValue(options.getPagingState().serialize(), dest);
            if (flags.contains(Flag.SERIAL_CONSISTENCY))
                CBUtil.writeConsistencyLevel(options.getSerialConsistency(), dest);
            if (flags.contains(Flag.TIMESTAMP))
                dest.writeLong(options.getSpecificOptions().timestamp);

            // Note that we don't really have to bother with NAMES_FOR_VALUES server side,
            // and in fact we never really encode QueryOptions, only decode them, so we
            // don't bother.
        }

        public int encodedSize(QueryOptions options, int version)
        {
            int size = 0;

            size += CBUtil.sizeOfConsistencyLevel(options.getConsistency());

            EnumSet<Flag> flags = gatherFlags(options);
            size += 1;

            if (flags.contains(Flag.VALUES))
                size += CBUtil.sizeOfValueList(options.getValues());
            if (flags.contains(Flag.PAGE_SIZE))
                size += 4;
            if (flags.contains(Flag.PAGING_STATE))
                size += CBUtil.sizeOfValue(options.getPagingState().serialize());
            if (flags.contains(Flag.SERIAL_CONSISTENCY))
                size += CBUtil.sizeOfConsistencyLevel(options.getSerialConsistency());
            if (flags.contains(Flag.TIMESTAMP))
                size += 8;

            return size;
        }

        private EnumSet<Flag> gatherFlags(QueryOptions options)
        {
            EnumSet<Flag> flags = EnumSet.noneOf(Flag.class);
            if (options.getValues().size() > 0)
                flags.add(Flag.VALUES);
            if (options.skipMetadata())
                flags.add(Flag.SKIP_METADATA);
            if (options.getPageSize() >= 0)
                flags.add(Flag.PAGE_SIZE);
            if (options.getPagingState() != null)
                flags.add(Flag.PAGING_STATE);
            if (options.getSerialConsistency() != ConsistencyLevel.SERIAL)
                flags.add(Flag.SERIAL_CONSISTENCY);
            if (options.getSpecificOptions().timestamp != Long.MIN_VALUE)
                flags.add(Flag.TIMESTAMP);
            return flags;
        }
    }
#endif
};

class default_query_options : public query_options {
private:
    const db::consistency_level _consistency;
    const std::vector<bytes_opt> _values;
    const bool _skip_metadata;
    const specific_options _options;
    const int32_t _protocol_version; // transient
public:
    default_query_options(db::consistency_level consistency, std::vector<bytes_opt> values, bool skip_metadata, specific_options options,
        int32_t protocol_version, serialization_format sf)
        : query_options(sf)
        , _consistency(consistency)
        , _values(std::move(values))
        , _skip_metadata(skip_metadata)
        , _options(std::move(options))
        , _protocol_version(protocol_version)
    { }
    virtual db::consistency_level get_consistency() const override {
        return _consistency;
    }
    virtual const std::vector<bytes_opt>& get_values() const override {
        return _values;
    }
    virtual bool skip_metadata() const override {
        return _skip_metadata;
    }
    virtual int32_t get_protocol_version() const override {
        return _protocol_version;
    }
    virtual const specific_options& get_specific_options() const override {
        return _options;
    }
};

class query_options_wrapper : public query_options {
protected:
    std::unique_ptr<query_options> _wrapped;
public:
    query_options_wrapper(std::unique_ptr<query_options> wrapped)
            : query_options(wrapped->get_serialization_format())
            , _wrapped(std::move(wrapped)) {
    }

    virtual db::consistency_level get_consistency() const override {
        return _wrapped->get_consistency();
    }

    virtual const std::vector<bytes_opt>& get_values() const override {
        return _wrapped->get_values();
    }

    virtual bool skip_metadata() const override {
        return _wrapped->skip_metadata();
    }

    virtual int get_protocol_version() const override {
        return _wrapped->get_protocol_version();
    }

    virtual const specific_options& get_specific_options() const override {
        return _wrapped->get_specific_options();
    }

    virtual void prepare(const std::vector<::shared_ptr<column_specification>>& specs) override {
        _wrapped->prepare(specs);
    }
};

class options_with_names : public query_options_wrapper {
private:
    std::vector<sstring> _names;
    std::vector<bytes_opt> _ordered_values;
public:
    options_with_names(std::unique_ptr<query_options> wrapped, std::vector<sstring> names)
        : query_options_wrapper(std::move(wrapped))
        , _names(std::move(names))
    { }

    void prepare(const std::vector<::shared_ptr<column_specification>>& specs) override {
        query_options::prepare(specs);

        _ordered_values.resize(specs.size());
        auto& wrapped_values = _wrapped->get_values();

        for (auto&& spec : specs) {
            auto& spec_name = spec->name->text();
            for (size_t j = 0; j < _names.size(); j++) {
                if (_names[j] == spec_name) {
                    _ordered_values.emplace_back(wrapped_values[j]);
                    break;
                }
            }
        }
    }

    virtual const std::vector<bytes_opt>& get_values() const override {
        return _ordered_values;
    }
};

}

#endif
