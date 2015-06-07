/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once
#include "bytes.hh"
#include "schema.hh"
#include <boost/any.hpp>
#include "core/future.hh"

class partition_key;
class clustering_key;

namespace sstables {

class key_view {
    bytes_view _bytes;
public:
    key_view(bytes_view b) : _bytes(b) {}
    key_view() : _bytes() {}

    std::vector<bytes> explode(const schema& s) const;

    bool operator==(const key_view& k) const { return k._bytes == _bytes; }
    bool operator!=(const key_view& k) const { return !(k == *this); }

    bool empty() { return _bytes.empty(); }

    explicit operator bytes_view() const {
        return _bytes;
    }
};

enum class composite_marker : bytes::value_type {
    start_range = -1,
    none = 0,
    end_range = 1,
};

inline void check_marker(bytes_view component, composite_marker expected) {
    auto found = composite_marker(component.back());
    if (found != expected) {
        throw runtime_exception(sprint("Unexpected marker. Found %d, expected %d\n", uint8_t(found), uint8_t(expected)));
    }
}

// Our internal representation differs slightly (in the way it serializes) from Origin.
// In order to be able to achieve read and write compatibility for sstables - so they can
// be imported and exported - we need to always convert a key to this representation.
class key {
    enum class kind {
        before_all_keys,
        regular,
        after_all_keys,
    };
    kind _kind;
    bytes _bytes;
public:
    key(bytes&& b) : _kind(kind::regular), _bytes(std::move(b)) {}
    key(kind k) : _kind(k) {}
    static key from_bytes(bytes b) { return key(std::move(b)); }
    static key from_deeply_exploded(const schema& s, const std::vector<boost::any>& v);
    static key from_exploded(const schema& s, const std::vector<bytes>& v);
    static key from_exploded(const schema& s, std::vector<bytes>&& v);
    // Unfortunately, the _bytes field for the partition_key are not public. We can't move.
    static key from_partition_key(const schema& s, const partition_key& pk);

    std::vector<bytes> explode(const schema& s) const;

    int32_t tri_compare(key_view k) const {
        if (_kind == kind::before_all_keys) {
            return -1;
        }
        if (_kind == kind::after_all_keys) {
            return 1;
        }
        return compare_unsigned(bytes_view(_bytes), bytes_view(k));
    }
    operator key_view() const {
        return key_view(_bytes);
    }
    explicit operator bytes_view() const {
        return _bytes;
    }
    bytes& get_bytes() {
        return _bytes;
    }
    friend key minimum_key();
    friend key maximum_key();
};

inline key minimum_key() {
    return key(key::kind::before_all_keys);
};

inline key maximum_key() {
    return key(key::kind::after_all_keys);
};

class composite_view {
    bytes_view _bytes;
public:
    composite_view(bytes_view b) : _bytes(b) {}

    std::vector<bytes> explode() const;

    explicit operator bytes_view() const {
        return _bytes;
    }
};

class composite {
    bytes _bytes;
public:
    composite (bytes&& b) : _bytes(std::move(b)) {}
    template <typename Describer>
    future<> describe_type(Describer f) const { return f(const_cast<bytes&>(_bytes)); }

    static composite from_bytes(bytes b) { return composite(std::move(b)); }
    template <typename ClusteringElement>
    static composite from_clustering_element(const schema& s, const ClusteringElement& ce);
    static composite from_exploded(const std::vector<bytes_view>& v, composite_marker m = composite_marker::none);
    static composite static_prefix(const schema& s);
    size_t size() const { return _bytes.size(); }
    explicit operator bytes_view() const {
        return _bytes;
    }
    operator composite_view() const {
        return composite_view(_bytes);
    }
};
}
