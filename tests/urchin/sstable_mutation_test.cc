/*
 * Copyright 2015 Cloudius Systems
 */

#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>
#include "tests/test-utils.hh"
#include "sstable_test.hh"
#include "sstables/key.hh"
#include "core/do_with.hh"
#include "database.hh"
#include "timestamp.hh"

using namespace sstables;

SEASTAR_TEST_CASE(nonexistent_key) {
    return reusable_sst("tests/urchin/sstables/uncompressed", 1).then([] (auto sstp) {
        return do_with(key::from_bytes(to_bytes("invalid_key")), [sstp] (auto& key) {
            auto s = uncompressed_schema();
            return sstp->convert_row(s, key).then([sstp, s, &key] (auto mutation) {
                BOOST_REQUIRE(!mutation);
                return make_ready_future<>();
            });
        });
    });
}

enum class status {
    dead,
    live,
    ttl,
};

inline bool check_status_and_done(const atomic_cell &c, status expected) {
    if (expected == status::dead) {
        BOOST_REQUIRE(c.is_live() == false);
        return true;
    }
    BOOST_REQUIRE(c.is_live() == true);
    BOOST_REQUIRE(c.is_live_and_has_ttl() == (expected == status::ttl));
    return false;
}

template <status Status>
void match(const row& row, const schema& s, bytes col, const boost::any& value, int64_t timestamp = 0, int32_t expiration = 0) {
    auto cdef = s.get_column_definition(col);

    BOOST_CHECK_NO_THROW(row.cell_at(cdef->id));
    auto c = row.cell_at(cdef->id).as_atomic_cell();
    if (check_status_and_done(c, Status)) {
        return;
    }

    auto expected = cdef->type->decompose(value);
    BOOST_REQUIRE(c.value() == expected);
    if (timestamp) {
        BOOST_REQUIRE(c.timestamp() == timestamp);
    }
    if (expiration) {
        BOOST_REQUIRE(c.expiry() == gc_clock::time_point(gc_clock::duration(expiration)));
    }
}

void match_live_cell(const row& row, const schema& s, bytes col, const boost::any& value) {
    match<status::live>(row, s, col, value);
}

void match_expiring_cell(const row& row, const schema& s, bytes col, const boost::any& value, int64_t timestamp, int32_t expiration) {
    match<status::ttl>(row, s, col, value);
}

void match_dead_cell(const row& row, const schema& s, bytes col) {
    match<status::dead>(row, s, col, boost::any({}));
}

void match_absent(const row& row, const schema& s, bytes col) {
    auto cdef = s.get_column_definition(col);
    BOOST_REQUIRE_THROW(row.cell_at(cdef->id), std::out_of_range);
}

inline collection_type_impl::mutation
match_collection(const row& row, const schema& s, bytes col, const tombstone& t) {
    auto cdef = s.get_column_definition(col);

    BOOST_CHECK_NO_THROW(row.cell_at(cdef->id));
    auto c = row.cell_at(cdef->id).as_collection_mutation();
    auto ctype = static_pointer_cast<const collection_type_impl>(cdef->type);
    auto&& mut = ctype->deserialize_mutation_form(c);
    BOOST_REQUIRE(mut.tomb == t);
    return mut.materialize();
}

template <status Status>
void match_collection_element(const std::pair<bytes, atomic_cell>& element, const bytes_opt& col, const bytes_opt& expected_serialized_value) {
    if (col) {
        BOOST_REQUIRE(element.first == *col);
    }

    if (check_status_and_done(element.second, Status)) {
        return;
    }

    // For simplicity, we will have all set elements in our schema presented as
    // bytes - which serializes to itself.  Then we don't need to meddle with
    // the schema for the set type, and is enough for the purposes of this
    // test.
    if (expected_serialized_value) {
        BOOST_REQUIRE(element.second.value() == *expected_serialized_value);
    }
}

future<> test_no_clustered(bytes&& key, std::unordered_map<bytes, boost::any> &&map) {
    return reusable_sst("tests/urchin/sstables/uncompressed", 1).then([k = std::move(key), map = std::move(map)] (auto sstp) mutable {
        return do_with(sstables::key(std::move(k)), [sstp, map = std::move(map)] (auto& key) {
            auto s = uncompressed_schema();
            return sstp->convert_row(s, key).then([sstp, s, &key, map = std::move(map)] (auto mutation) {
                BOOST_REQUIRE(mutation);
                auto& mp = mutation->partition();
                for (auto&& e : mp.range(*s, query::range<clustering_key_prefix>())) {
                    BOOST_REQUIRE(to_bytes(e.key()) == to_bytes(""));
                    BOOST_REQUIRE(e.row().cells().size() == map.size());

                    auto &row = e.row().cells();
                    for (auto&& c: map) {
                        match_live_cell(row, *s, c.first, c.second);
                    }
                }
                return make_ready_future<>();
            });
        });
    });
}

SEASTAR_TEST_CASE(uncompressed_1) {
    return test_no_clustered("vinna", {{ "col1", boost::any(to_sstring("daughter")) }, { "col2", boost::any(3) }});
}

SEASTAR_TEST_CASE(uncompressed_2) {
    return test_no_clustered("gustaf", {{ "col1", boost::any(to_sstring("son")) }, { "col2", boost::any(0) }});
}

SEASTAR_TEST_CASE(uncompressed_3) {
    return test_no_clustered("isak", {{ "col1", boost::any(to_sstring("son")) }, { "col2", boost::any(1) }});
}

SEASTAR_TEST_CASE(uncompressed_4) {
    return test_no_clustered("finna", {{ "col1", boost::any(to_sstring("daughter")) }, { "col2", boost::any(2) }});
}

/*
 *
 * insert into todata.complex_schema (key, clust1, clust2, reg_set, reg, static_obj) values ('key1', 'cl1.1', 'cl2.1', { '1', '2' }, 'v1', 'static_value');
 * insert into todata.complex_schema (key, clust1, clust2, reg_list, reg, static_obj) values ('key1', 'cl1.2', 'cl2.2', [ '2', '1'], 'v2','static_value');
 * insert into todata.complex_schema (key, clust1, clust2, reg_map, reg, static_obj) values ('key2', 'kcl1.1', 'kcl2.1', { '3': '1', '4' : '2' }, 'v3', 'static_value');
 * insert into todata.complex_schema (key, clust1, clust2, reg_fset, reg, static_obj) values ('key2', 'kcl1.2', 'kcl2.2', { '3', '1', '4' , '2' }, 'v4', 'static_value');
 * insert into todata.complex_schema (key, static_collection) values ('key2', { '1', '2', '3' , '4' });
 * (flush)
 *
 * delete reg from todata.complex_schema where key = 'key2' and clust1 = 'kcl1.2' and clust2 = 'kcl2.2';
 * insert into todata.complex_schema (key, clust1, clust2, reg, static_obj) values ('key3', 'tcl1.1', 'tcl2.1', 'v5', 'static_value_3') using ttl 86400;
 * delete from todata.complex_schema where key = 'key1' and clust1='cl1.1';
 * delete static_obj from todata.complex_schema where key = 'key2';
 * delete reg_list[0] from todata.complex_schema where key = 'key1' and clust1='cl1.2' and clust2='cl2.2';
 * delete reg_fset from todata.complex_schema where key = 'key2' and clust1='kcl1.2' and clust2='kcl2.2';
 * delete reg_map['3'] from todata.complex_schema where key = 'key2' and clust1='kcl1.1' and clust2='kcl2.1';
 * delete static_collection['1'] from todata.complex_schema where key = 'key2';
 * (flush)
 *
 * insert into todata.complex_schema (key, static_obj) values('key2', 'final_static');
 * update todata.complex_schema set reg_map = reg_map + { '6': '1' } where key = 'key2' and clust1='kcl1.1' and clust2='kcl2.1';
 * update todata.complex_schema set reg_list = reg_list + [ '6' ] where key = 'key1' and clust1='cl1.2' and clust2='cl2.2';
 * update todata.complex_schema set reg_set = reg_set + { '6' } where key = 'key1' and clust1='cl1.2' and clust2='cl2.2';
 * (flush)
 */

// FIXME: we are lacking a full deletion test
template <int Generation>
future<mutation> generate_clustered(bytes&& key) {
    return reusable_sst("tests/urchin/sstables/complex", Generation).then([k = std::move(key)] (auto sstp) mutable {
        return do_with(sstables::key(std::move(k)), [sstp] (auto& key) {
            auto s = complex_schema();
            return sstp->convert_row(s, key).then([sstp, s, &key] (auto mutation) {
                BOOST_REQUIRE(mutation);
                return std::move(*mutation);
            });
        });
    });
}

inline auto clustered_row(mutation& mutation, const schema& s, std::vector<bytes>&& v) {
    auto exploded = exploded_clustering_prefix(std::move(v));
    auto clustering_pair = clustering_key::from_clustering_prefix(s, exploded);
    return mutation.partition().clustered_row(clustering_pair);
}

SEASTAR_TEST_CASE(complex_sst1_k1) {
    return generate_clustered<1>("key1").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto sr = mutation.partition().static_row();
        match_live_cell(sr, *s, "static_obj", boost::any(to_bytes("static_value")));

        auto row1 = clustered_row(mutation, *s, {"cl1.1", "cl2.1"});
        match_live_cell(row1.cells(), *s, "reg", boost::any(to_bytes("v1")));
        match_absent(row1.cells(), *s, "reg_list");
        match_absent(row1.cells(), *s, "reg_map");
        match_absent(row1.cells(), *s, "reg_fset");
        auto reg_set = match_collection(row1.cells(), *s, "reg_set", tombstone(deletion_time{1431451390, 1431451390209521l}));
        match_collection_element<status::live>(reg_set.cells[0], to_bytes("1"), bytes_opt{});
        match_collection_element<status::live>(reg_set.cells[1], to_bytes("2"), bytes_opt{});

        auto row2 = clustered_row(mutation, *s, {"cl1.2", "cl2.2"});
        match_live_cell(row2.cells(), *s, "reg", boost::any(to_bytes("v2")));
        match_absent(row2.cells(), *s, "reg_set");
        match_absent(row2.cells(), *s, "reg_map");
        match_absent(row2.cells(), *s, "reg_fset");
        auto reg_list = match_collection(row2.cells(), *s, "reg_list", tombstone(deletion_time{1431451390, 1431451390213471l}));
        match_collection_element<status::live>(reg_list.cells[0], bytes_opt{}, to_bytes("2"));
        match_collection_element<status::live>(reg_list.cells[1], bytes_opt{}, to_bytes("1"));

        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst1_k2) {
    return generate_clustered<1>("key2").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto sr = mutation.partition().static_row();
        match_live_cell(sr, *s, "static_obj", boost::any(to_bytes("static_value")));
        auto static_set = match_collection(sr, *s, "static_collection", tombstone(deletion_time{1431451390, 1431451390225257l}));
        match_collection_element<status::live>(static_set.cells[0], to_bytes("1"), bytes_opt{});
        match_collection_element<status::live>(static_set.cells[1], to_bytes("2"), bytes_opt{});
        match_collection_element<status::live>(static_set.cells[2], to_bytes("3"), bytes_opt{});
        match_collection_element<status::live>(static_set.cells[3], to_bytes("4"), bytes_opt{});

        auto row1 = clustered_row(mutation, *s, {"kcl1.1", "kcl2.1"});
        match_live_cell(row1.cells(), *s, "reg", boost::any(to_bytes("v3")));
        match_absent(row1.cells(), *s, "reg_list");
        match_absent(row1.cells(), *s, "reg_set");
        match_absent(row1.cells(), *s, "reg_fset");
        auto reg_map = match_collection(row1.cells(), *s, "reg_map", tombstone(deletion_time{1431451390, 1431451390217436l}));
        match_collection_element<status::live>(reg_map.cells[0], to_bytes("3"), to_bytes("1"));
        match_collection_element<status::live>(reg_map.cells[1], to_bytes("4"), to_bytes("2"));

        auto row2 = clustered_row(mutation, *s, {"kcl1.2", "kcl2.2"});
        match_live_cell(row2.cells(), *s, "reg", boost::any(to_bytes("v4")));
        match_absent(row2.cells(), *s, "reg_set");
        match_absent(row2.cells(), *s, "reg_map");
        match_absent(row2.cells(), *s, "reg_list");
 
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst2_k1) {
    return generate_clustered<2>("key1").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto exploded = exploded_clustering_prefix({"cl1.1", "cl2.1"});
        auto clustering = clustering_key::from_clustering_prefix(*s, exploded);

        auto t1 = mutation.partition().range_tombstone_for_row(*s, clustering);
        BOOST_REQUIRE(t1.timestamp == 1431451394600754l);
        BOOST_REQUIRE(t1.deletion_time == gc_clock::time_point(gc_clock::duration(1431451394)));

        auto row = clustered_row(mutation, *s, {"cl1.2", "cl2.2"});
        auto reg_list = match_collection(row.cells(), *s, "reg_list", tombstone(deletion_time{0, api::missing_timestamp}));
        match_collection_element<status::dead>(reg_list.cells[0], bytes_opt{}, bytes_opt{});
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst2_k2) {
    return generate_clustered<2>("key2").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto sr = mutation.partition().static_row();
        match_dead_cell(sr, *s, "static_obj");
        auto static_set = match_collection(sr, *s, "static_collection", tombstone(deletion_time{0, api::missing_timestamp}));
        match_collection_element<status::dead>(static_set.cells[0], to_bytes("1"), bytes_opt{});

        auto row1 = clustered_row(mutation, *s, {"kcl1.1", "kcl2.1"});
        // map dead
        match_absent(row1.cells(), *s, "reg_list");
        match_absent(row1.cells(), *s, "reg_set");
        match_absent(row1.cells(), *s, "reg_fset");
        match_absent(row1.cells(), *s, "reg");
        match_collection(row1.cells(), *s, "reg_map", tombstone(deletion_time{0, api::missing_timestamp}));

        auto row2 = clustered_row(mutation, *s, {"kcl1.2", "kcl2.2"});
        match_dead_cell(row2.cells(), *s, "reg");
        match_absent(row2.cells(), *s, "reg_map");
        match_absent(row2.cells(), *s, "reg_list");
        match_absent(row2.cells(), *s, "reg_set");
        match_dead_cell(row2.cells(), *s, "reg_fset");
        match_dead_cell(row2.cells(), *s, "reg");

        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst2_k3) {
    return generate_clustered<2>("key3").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto sr = mutation.partition().static_row();
        match_expiring_cell(sr, *s, "static_obj", boost::any(to_bytes("static_value_3")), 1431451394597062l, 1431537794);

        auto row1 = clustered_row(mutation, *s, {"tcl1.1", "tcl2.1"});
        BOOST_REQUIRE(row1.created_at() == 1431451394597062l);
        match_expiring_cell(row1.cells(), *s, "reg", boost::any(to_bytes("v5")), 1431451394597062l, 1431537794);
        match_absent(row1.cells(), *s, "reg_list");
        match_absent(row1.cells(), *s, "reg_set");
        match_absent(row1.cells(), *s, "reg_map");
        match_absent(row1.cells(), *s, "reg_fset");
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst3_k1) {
    return generate_clustered<3>("key1").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto row = clustered_row(mutation, *s, {"cl1.2", "cl2.2"});

        auto reg_set = match_collection(row.cells(), *s, "reg_set", tombstone(deletion_time{0, api::missing_timestamp}));
        match_collection_element<status::live>(reg_set.cells[0], to_bytes("6"), bytes_opt{});

        auto reg_list = match_collection(row.cells(), *s, "reg_list", tombstone(deletion_time{0, api::missing_timestamp}));
        match_collection_element<status::live>(reg_list.cells[0], bytes_opt{}, to_bytes("6"));

        match_absent(row.cells(), *s, "static_obj");
        match_absent(row.cells(), *s, "reg_map");
        match_absent(row.cells(), *s, "reg");
        match_absent(row.cells(), *s, "reg_fset");
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(complex_sst3_k2) {
    return generate_clustered<3>("key2").then([] (auto&& mutation) {
        auto s = complex_schema();

        auto sr = mutation.partition().static_row();
        match_live_cell(sr, *s, "static_obj", boost::any(to_bytes("final_static")));

        auto row = clustered_row(mutation, *s, {"kcl1.1", "kcl2.1"});
        auto reg_map = match_collection(row.cells(), *s, "reg_map", tombstone(deletion_time{0, api::missing_timestamp}));
        match_collection_element<status::live>(reg_map.cells[0], to_bytes("6"), to_bytes("1"));
        match_absent(row.cells(), *s, "reg_list");
        match_absent(row.cells(), *s, "reg_set");
        match_absent(row.cells(), *s, "reg");
        match_absent(row.cells(), *s, "reg_fset");
        return make_ready_future<>();
    });
}
