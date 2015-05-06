/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <iostream>

#include "keys.hh"

std::ostream& operator<<(std::ostream& out, const partition_key& pk) {
    return out << "pk{" << to_hex(pk) << "}";
}

std::ostream& operator<<(std::ostream& out, const clustering_key& ck) {
    return out << "ck{" << to_hex(ck) << "}";
}

std::ostream& operator<<(std::ostream& out, const clustering_key_prefix& ckp) {
    return out << "ckp{" << to_hex(ckp) << "}";
}

const legacy_compound_view<partition_key_view::c_type>
partition_key_view::legacy_form(const schema& s) const {
    return { *get_compound_type(s), _bytes };
}

int
partition_key_view::legacy_tri_compare(const schema& s, const partition_key& o) const {
    auto cmp = legacy_compound_view<c_type>::tri_comparator(*get_compound_type(s));
    return cmp(this->representation(), o.representation());
}
