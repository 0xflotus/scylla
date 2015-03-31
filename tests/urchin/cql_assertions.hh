#pragma once

#include <boost/test/included/unit_test.hpp>
#include "transport/messages/result_message.hh"
#include "to_string.hh"
#include "bytes.hh"

class rows_assertions {
    shared_ptr<transport::messages::result_message::rows> _rows;
public:
    rows_assertions(shared_ptr<transport::messages::result_message::rows> rows)
        : _rows(rows)
    { }

    rows_assertions with_size(size_t size) {
        auto row_count = _rows->rs().size();
        if (row_count != size) {
            BOOST_FAIL(sprint("Expected %d row(s) but got %d", size, row_count));
        }
        return {*this};
    }

    rows_assertions is_empty() {
        auto row_count = _rows->rs().size();
        if (row_count != 0) {
            auto&& first_row = *_rows->rs().rows().begin();
            BOOST_FAIL(sprint("Expected no rows, but got %d. First row: %s", row_count, to_string(first_row)));
        }
        return {*this};
    }

    rows_assertions with_row(std::initializer_list<bytes_opt> values) {
        std::vector<bytes_opt> expected_row(values);
        for (auto&& row : _rows->rs().rows()) {
            if (row == expected_row) {
                return {*this};
            }
        }
        BOOST_FAIL("Expected row not found");
        return {*this};
    }

    // Verifies that the result has the following rows and only that rows, in that order.
    rows_assertions with_rows(std::initializer_list<std::initializer_list<bytes_opt>> rows) {
        auto actual_i = _rows->rs().rows().begin();
        auto actual_end = _rows->rs().rows().end();
        int row_nr = 0;
        for (auto&& row : rows) {
            if (actual_i == actual_end) {
                BOOST_FAIL(sprint("Expected more rows (%d), got %d", rows.size(), _rows->rs().size()));
            }
            auto& actual = *actual_i;
            if (!std::equal(
                    std::begin(row), std::end(row),
                    std::begin(actual), std::end(actual))) {
                BOOST_FAIL(sprint("row %d differs, expected %s got %s", row_nr, to_string(row), to_string(actual)));
            }
            ++actual_i;
            ++row_nr;
        }
        if (actual_i != actual_end) {
            BOOST_FAIL(sprint("Expected less rows (%d), got %d. Next row is: %s", rows.size(), _rows->rs().size(),
                to_string(*actual_i)));
        }
        return {*this};
    }
};

class result_msg_assertions {
    shared_ptr<transport::messages::result_message> _msg;
public:
    result_msg_assertions(shared_ptr<transport::messages::result_message> msg)
        : _msg(msg)
    { }

    rows_assertions is_rows() {
        auto rows = dynamic_pointer_cast<transport::messages::result_message::rows>(_msg);
        BOOST_REQUIRE(rows);
        return rows_assertions(rows);
    }
};

static inline
result_msg_assertions assert_that(shared_ptr<transport::messages::result_message> msg) {
    return result_msg_assertions(msg);
}
