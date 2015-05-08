/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "mutation_partition_view.hh"
#include "schema.hh"
#include "atomic_cell.hh"
#include "db/serializer.hh"
#include "utils/data_input.hh"
#include "mutation_partition_serializer.hh"

//
// See mutation_partition_serializer.cc for representation layout.
//

using namespace db;

void
mutation_partition_view::accept(const schema& schema, mutation_partition_visitor& visitor) const {
    data_input in(_bytes);

    visitor.accept_partition_tombstone(tombstone_serializer::read(in));

    // Read static row
    auto n_columns = in.read<mutation_partition_serializer::count_type>();
    while (n_columns-- > 0) {
        auto id = in.read<column_id>();

        if (schema.static_column_at(id).is_atomic()) {
            auto&& v = atomic_cell_view_serializer::read(in);
            visitor.accept_static_cell(id, v);
        } else {
            auto&& v = collection_mutation_view_serializer::read(in);
            visitor.accept_static_cell(id, v);
        }
    }

    // Read row tombstones
    auto n_tombstones = in.read<mutation_partition_serializer::count_type>();
    while (n_tombstones-- > 0) {
        auto&& prefix = clustering_key_prefix_view_serializer::read(in);
        auto&& t = tombstone_serializer::read(in);
        visitor.accept_row_tombstone(prefix, t);
    }

    // Read clustered rows
    while (in.has_next()) {
        auto&& key = clustering_key_view_serializer::read(in);
        auto&& created_at = in.read<api::timestamp_type>();
        auto&& deleted_at = tombstone_serializer::read(in);
        visitor.accept_row(key, created_at, deleted_at);

        auto n_columns = in.read<mutation_partition_serializer::count_type>();
        while (n_columns-- > 0) {
            auto id = in.read<column_id>();

            if (schema.regular_column_at(id).is_atomic()) {
                auto&& v = atomic_cell_view_serializer::read(in);
                visitor.accept_row_cell(id, v);
            } else {
                auto&& v = collection_mutation_view_serializer::read(in);
                visitor.accept_row_cell(id, v);
            }
        }
    }
}
