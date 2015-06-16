/*
 * Copyright 2015 Cloudius Systems
 */

#include "log.hh"
#include <vector>
#include <typeinfo>
#include <limits>
#include "core/future.hh"
#include "core/future-util.hh"
#include "core/sstring.hh"
#include "core/fstream.hh"
#include "core/shared_ptr.hh"
#include "core/do_with.hh"
#include "core/thread.hh"
#include <iterator>

#include "types.hh"
#include "sstables.hh"
#include "compress.hh"
#include "unimplemented.hh"
#include <boost/algorithm/string.hpp>

namespace sstables {

class random_access_reader {
    input_stream<char> _in;
protected:
    virtual input_stream<char> open_at(uint64_t pos) = 0;
public:
    future<temporary_buffer<char>> read_exactly(size_t n) {
        return _in.read_exactly(n);
    }
    void seek(uint64_t pos) {
        _in = open_at(pos);
    }
    bool eof() { return _in.eof(); }
    virtual ~random_access_reader() { }
};

class file_random_access_reader : public random_access_reader {
    lw_shared_ptr<file> _file;
    size_t _buffer_size;
public:
    virtual input_stream<char> open_at(uint64_t pos) override {
        return make_file_input_stream(_file, pos, _buffer_size);
    }
    explicit file_random_access_reader(file&& f, size_t buffer_size = 8192)
        : file_random_access_reader(make_lw_shared<file>(std::move(f)), buffer_size) {}

    explicit file_random_access_reader(lw_shared_ptr<file> f, size_t buffer_size = 8192)
        : _file(f), _buffer_size(buffer_size)
    {
        seek(0);
    }
};

// FIXME: We don't use this API, and it can be removed. The compressed reader
// is only needed for the data file, and for that we have the nicer
// data_stream_at() API below.
class compressed_file_random_access_reader : public random_access_reader {
    lw_shared_ptr<file> _file;
    sstables::compression* _cm;
public:
    explicit compressed_file_random_access_reader(
                lw_shared_ptr<file> f, sstables::compression* cm)
        : _file(std::move(f))
        , _cm(cm)
    {
        seek(0);
    }
    compressed_file_random_access_reader(compressed_file_random_access_reader&&) = default;
    virtual input_stream<char> open_at(uint64_t pos) override {
        return make_compressed_file_input_stream(_file, _cm, pos);
    }

};

thread_local logging::logger sstlog("sstable");

std::unordered_map<sstable::version_types, sstring, enum_hash<sstable::version_types>> sstable::_version_string = {
    { sstable::version_types::la , "la" }
};

std::unordered_map<sstable::format_types, sstring, enum_hash<sstable::format_types>> sstable::_format_string = {
    { sstable::format_types::big , "big" }
};

std::unordered_map<sstable::component_type, sstring, enum_hash<sstable::component_type>> sstable::_component_map = {
    { component_type::Index, "Index.db"},
    { component_type::CompressionInfo, "CompressionInfo.db" },
    { component_type::Data, "Data.db" },
    { component_type::TOC, "TOC.txt" },
    { component_type::Summary, "Summary.db" },
    { component_type::Digest, "Digest.sha1" },
    { component_type::CRC, "CRC.db" },
    { component_type::Filter, "Filter.db" },
    { component_type::Statistics, "Statistics.db" },
};

// This assumes that the mappings are small enough, and called unfrequent
// enough.  If that changes, it would be adviseable to create a full static
// reverse mapping, even if it is done at runtime.
template <typename Map>
static typename Map::key_type reverse_map(const typename Map::mapped_type& value, Map& map) {
    for (auto& pair: map) {
        if (pair.second == value) {
            return pair.first;
        }
    }
    throw std::out_of_range("unable to reverse map");
}

struct bufsize_mismatch_exception : malformed_sstable_exception {
    bufsize_mismatch_exception(size_t size, size_t expected) :
        malformed_sstable_exception(sprint("Buffer improperly sized to hold requested data. Got: %ld. Expected: %ld", size, expected))
    {}
};

// This should be used every time we use read_exactly directly.
//
// read_exactly is a lot more convenient of an interface to use, because we'll
// be parsing known quantities.
//
// However, anything other than the size we have asked for, is certainly a bug,
// and we need to do something about it.
static void check_buf_size(temporary_buffer<char>& buf, size_t expected) {
    if (buf.size() < expected) {
        throw bufsize_mismatch_exception(buf.size(), expected);
    }
}

template <typename T, typename U>
static void check_truncate_and_assign(T& to, const U from) {
    static_assert(std::is_integral<T>::value && std::is_integral<U>::value, "T and U must be integral");
    to = from;
    if (to != from) {
        throw std::overflow_error("assigning U to T caused an overflow");
    }
}

// Base parser, parses an integer type
template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, void>
read_integer(temporary_buffer<char>& buf, T& i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(buf.get());
    i = net::ntoh(*nr);
}

template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, future<>>
parse(random_access_reader& in, T& i) {
    return in.read_exactly(sizeof(T)).then([&i] (auto buf) {
        check_buf_size(buf, sizeof(T));

        read_integer(buf, i);
        return make_ready_future<>();
    });
}

template <typename T>
inline typename std::enable_if_t<std::is_integral<T>::value, void>
write(file_writer& out, T i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(&i);
    i = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&i);
    out.write(p, sizeof(T)).get();
}

template <typename T>
typename std::enable_if_t<std::is_enum<T>::value, future<>>
parse(random_access_reader& in, T& i) {
    return parse(in, reinterpret_cast<typename std::underlying_type<T>::type&>(i));
}

template <typename T>
inline typename std::enable_if_t<std::is_enum<T>::value, void>
write(file_writer& out, T i) {
    write(out, static_cast<typename std::underlying_type<T>::type>(i));
}

future<> parse(random_access_reader& in, bool& i) {
    return parse(in, reinterpret_cast<uint8_t&>(i));
}

inline void write(file_writer& out, bool i) {
    write(out, static_cast<uint8_t>(i));
}

template <typename To, typename From>
static inline To convert(From f) {
    static_assert(sizeof(To) == sizeof(From), "Sizes must match");
    union {
        To to;
        From from;
    } conv;

    conv.from = f;
    return conv.to;
}

future<> parse(random_access_reader& in, double& d) {
    return in.read_exactly(sizeof(double)).then([&d] (auto buf) {
        check_buf_size(buf, sizeof(double));

        auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(buf.get());
        d = convert<double>(net::ntoh(*nr));
        return make_ready_future<>();
    });
}

inline void write(file_writer& out, double d) {
    auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(&d);
    auto tmp = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&tmp);
    out.write(p, sizeof(unsigned long)).get();
}

template <typename T>
future<> parse(random_access_reader& in, T& len, bytes& s) {
    return in.read_exactly(len).then([&s, len] (auto buf) {
        check_buf_size(buf, len);
        // Likely a different type of char. Most bufs are unsigned, whereas the bytes type is signed.
        s = bytes(reinterpret_cast<const bytes::value_type *>(buf.get()), len);
    });
}

inline void write(file_writer& out, bytes& s) {
    out.write(s).get();
}

inline void write(file_writer& out, bytes_view s) {
    out.write(reinterpret_cast<const char*>(s.data()), s.size()).get();
}

// All composite parsers must come after this
template<typename First, typename... Rest>
future<> parse(random_access_reader& in, First& first, Rest&&... rest) {
    return parse(in, first).then([&in, &rest...] {
        return parse(in, std::forward<Rest>(rest)...);
    });
}

template<typename First, typename... Rest>
inline void write(file_writer& out, First& first, Rest&&... rest) {
    write(out, first);
    write(out, std::forward<Rest>(rest)...);
}

// Intended to be used for a type that describes itself through describe_type().
template <class T>
typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, future<>>
parse(random_access_reader& in, T& t) {
    return t.describe_type([&in] (auto&&... what) -> future<> {
        return parse(in, what...);
    });
}

template <class T>
inline typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, void>
write(file_writer& out, T& t) {
    t.describe_type([&out] (auto&&... what) -> void {
        write(out, std::forward<decltype(what)>(what)...);
    });
}

// For all types that take a size, we provide a template that takes the type
// alone, and another, separate one, that takes a size parameter as well, of
// type Size. This is because although most of the time the size and the data
// are contiguous, it is not always the case. So we want to have the
// flexibility of parsing them separately.
template <typename Size>
future<> parse(random_access_reader& in, disk_string<Size>& s) {
    auto len = std::make_unique<Size>();
    auto f = parse(in, *len);
    return f.then([&in, &s, len = std::move(len)] {
        return parse(in, *len, s.value);
    });
}

template <typename Size>
inline void write(file_writer& out, disk_string<Size>& s) {
    Size len = 0;
    check_truncate_and_assign(len, s.value.size());
    write(out, len);
    write(out, s.value);
}

template <typename Size>
inline void write(file_writer& out, disk_string_view<Size>& s) {
    Size len;
    check_truncate_and_assign(len, s.value.size());
    write(out, len, s.value);
}

// We cannot simply read the whole array at once, because we don't know its
// full size. We know the number of elements, but if we are talking about
// disk_strings, for instance, we have no idea how much of the stream each
// element will take.
//
// Sometimes we do know the size, like the case of integers. There, all we have
// to do is to convert each member because they are all stored big endian.
// We'll offer a specialization for that case below.
template <typename Size, typename Members>
typename std::enable_if_t<!std::is_integral<Members>::value, future<>>
parse(random_access_reader& in, Size& len, std::vector<Members>& arr) {

    auto count = make_lw_shared<size_t>(0);
    auto eoarr = [count, len] { return *count == len; };

    return do_until(eoarr, [count, &in, &arr] {
        return parse(in, arr[(*count)++]);
    });
}

template <typename Size, typename Members>
typename std::enable_if_t<std::is_integral<Members>::value, future<>>
parse(random_access_reader& in, Size& len, std::vector<Members>& arr) {
    return in.read_exactly(len * sizeof(Members)).then([&arr, len] (auto buf) {
        check_buf_size(buf, len * sizeof(Members));

        auto *nr = reinterpret_cast<const net::packed<Members> *>(buf.get());
        for (size_t i = 0; i < len; ++i) {
            arr[i] = net::ntoh(nr[i]);
        }
        return make_ready_future<>();
    });
}

// We resize the array here, before we pass it to the integer / non-integer
// specializations
template <typename Size, typename Members>
future<> parse(random_access_reader& in, disk_array<Size, Members>& arr) {
    auto len = std::make_unique<Size>();
    auto f = parse(in, *len);
    return f.then([&in, &arr, len = std::move(len)] {
        arr.elements.resize(*len);
        return parse(in, *len, arr.elements);
    });
}

template <typename Members>
inline typename std::enable_if_t<!std::is_integral<Members>::value, void>
write(file_writer& out, std::vector<Members>& arr) {
    for (auto& a : arr) {
        write(out, a);
    }
}

template <typename Members>
inline typename std::enable_if_t<std::is_integral<Members>::value, void>
write(file_writer& out, std::vector<Members>& arr) {
    std::vector<Members> tmp;
    tmp.resize(arr.size());
    // copy arr into tmp converting each entry into big-endian representation.
    auto *nr = reinterpret_cast<const net::packed<Members> *>(arr.data());
    for (size_t i = 0; i < arr.size(); i++) {
        tmp[i] = net::hton(nr[i]);
    }
    auto p = reinterpret_cast<const char*>(tmp.data());
    auto bytes = tmp.size() * sizeof(Members);
    out.write(p, bytes).get();
}

template <typename Size, typename Members>
inline void write(file_writer& out, disk_array<Size, Members>& arr) {
    Size len = 0;
    check_truncate_and_assign(len, arr.elements.size());
    write(out, len);
    write(out, arr.elements);
}

template <typename Size, typename Key, typename Value>
future<> parse(random_access_reader& in, Size& len, std::unordered_map<Key, Value>& map) {
    return do_with(Size(), [&in, len, &map] (Size& count) {
        auto eos = [len, &count] { return len == count++; };
        return do_until(eos, [len, &in, &map] {
            struct kv {
                Key key;
                Value value;
            };

            return do_with(kv(), [&in, &map] (auto& el) {
                return parse(in, el.key, el.value).then([&el, &map] {
                    map.emplace(el.key, el.value);
                });
            });
        });
    });
}

template <typename Size, typename Key, typename Value>
future<> parse(random_access_reader& in, disk_hash<Size, Key, Value>& h) {
    auto w = std::make_unique<Size>();
    auto f = parse(in, *w);
    return f.then([&in, &h, w = std::move(w)] {
        return parse(in, *w, h.map);
    });
}

template <typename Key, typename Value>
inline void write(file_writer& out, std::unordered_map<Key, Value>& map) {
    for (auto& val: map) {
        write(out, val.first, val.second);
    };
}

template <typename Size, typename Key, typename Value>
inline void write(file_writer& out, disk_hash<Size, Key, Value>& h) {
    Size len = 0;
    check_truncate_and_assign(len, h.map.size());
    write(out, len);
    write(out, h.map);
}

future<> parse(random_access_reader& in, summary& s) {
    using pos_type = typename decltype(summary::positions)::value_type;

    return parse(in, s.header.min_index_interval,
                     s.header.size,
                     s.header.memory_size,
                     s.header.sampling_level,
                     s.header.size_at_full_sampling).then([&in, &s] {
        return in.read_exactly(s.header.size * sizeof(pos_type)).then([&in, &s] (auto buf) {
            auto len = s.header.size * sizeof(pos_type);
            check_buf_size(buf, len);

            s.entries.resize(s.header.size);

            auto *nr = reinterpret_cast<const pos_type *>(buf.get());
            s.positions = std::vector<pos_type>(nr, nr + s.header.size);

            // Since the keys in the index are not sized, we need to calculate
            // the start position of the index i+1 to determine the boundaries
            // of index i. The "memory_size" field in the header determines the
            // total memory used by the map, so if we push it to the vector, we
            // can guarantee that no conditionals are used, and we can always
            // query the position of the "next" index.
            s.positions.push_back(s.header.memory_size);
        }).then([&in, &s] {
            in.seek(sizeof(summary::header) + s.header.memory_size);
            return parse(in, s.first_key, s.last_key);
        }).then([&in, &s] {

            in.seek(s.positions[0] + sizeof(summary::header));

            assert(s.positions.size() == (s.entries.size() + 1));

            auto idx = make_lw_shared<size_t>(0);
            return do_for_each(s.entries.begin(), s.entries.end(), [idx, &in, &s] (auto& entry) {
                auto pos = s.positions[(*idx)++];
                auto next = s.positions[*idx];

                auto entrysize = next - pos;

                return in.read_exactly(entrysize).then([&entry, entrysize] (auto buf) {
                    check_buf_size(buf, entrysize);

                    auto keysize = entrysize - 8;
                    entry.key = bytes(reinterpret_cast<const int8_t*>(buf.get()), keysize);
                    buf.trim_front(keysize);
                    // FIXME: This is a le read. We should make this explicit
                    entry.position = *(reinterpret_cast<const net::packed<uint64_t> *>(buf.get()));

                    return make_ready_future<>();
                });
            }).then([&s] {
                // Delete last element which isn't part of the on-disk format.
                s.positions.pop_back();
            });
        });
    });
}

inline void write(file_writer& out, summary_entry& entry) {
    // FIXME: summary entry is supposedly written in memory order, but that
    // would prevent portability of summary file between machines of different
    // endianness. We can treat it as little endian to preserve portability.
    write(out, entry.key);
    auto p = reinterpret_cast<const char*>(&entry.position);
    out.write(p, sizeof(uint64_t)).get();
}

inline void write(file_writer& out, summary& s) {
    using pos_type = typename decltype(summary::positions)::value_type;

    // NOTE: positions and entries must be stored in NATIVE BYTE ORDER, not BIG-ENDIAN.
    write(out, s.header.min_index_interval,
                  s.header.size,
                  s.header.memory_size,
                  s.header.sampling_level,
                  s.header.size_at_full_sampling);
    auto p = reinterpret_cast<const char*>(s.positions.data());
    out.write(p, sizeof(pos_type) * s.positions.size()).get();
    write(out, s.entries);
    write(out, s.first_key, s.last_key);
}

future<summary_entry&> sstable::read_summary_entry(size_t i) {
    // The last one is the boundary marker
    if (i >= (_summary.entries.size())) {
        throw std::out_of_range(sprint("Invalid Summary index: %ld", i));
    }

    return make_ready_future<summary_entry&>(_summary.entries[i]);
}

future<> parse(random_access_reader& in, index_entry& ie) {
    return parse(in, ie.key, ie.position, ie.promoted_index);
}

future<> parse(random_access_reader& in, deletion_time& d) {
    return parse(in, d.local_deletion_time, d.marked_for_delete_at);
}

template <typename Child>
future<> parse(random_access_reader& in, std::unique_ptr<metadata>& p) {
    p.reset(new Child);
    return parse(in, *static_cast<Child *>(p.get()));
}

template <typename Child>
inline void write(file_writer& out, std::unique_ptr<metadata>& p) {
    write(out, *static_cast<Child *>(p.get()));
}

future<> parse(random_access_reader& in, statistics& s) {
    return parse(in, s.hash).then([&in, &s] {
        return do_for_each(s.hash.map.begin(), s.hash.map.end(), [&in, &s] (auto val) mutable {
            in.seek(val.second);

            switch (val.first) {
                case metadata_type::Validation:
                    return parse<validation_metadata>(in, s.contents[val.first]);
                case metadata_type::Compaction:
                    return parse<compaction_metadata>(in, s.contents[val.first]);
                case metadata_type::Stats:
                    return parse<stats_metadata>(in, s.contents[val.first]);
                default:
                    sstlog.warn("Invalid metadata type at Statistics file: {} ", int(val.first));
                    return make_ready_future<>();
                }
        });
    });
}

inline void write(file_writer& out, statistics& s) {
    write(out, s.hash);
    struct kv {
        metadata_type key;
        uint32_t value;
    };
    // sort map by file offset value and store the result into a vector.
    // this is indeed needed because output stream cannot afford random writes.
    auto v = make_shared<std::vector<kv>>();
    v->reserve(s.hash.map.size());
    for (auto val : s.hash.map) {
        kv tmp = { val.first, val.second };
        v->push_back(tmp);
    }
    std::sort(v->begin(), v->end(), [] (kv i, kv j) { return i.value < j.value; });
    for (auto& val: *v) {
        switch (val.key) {
            case metadata_type::Validation:
                write<validation_metadata>(out, s.contents[val.key]);
                break;
            case metadata_type::Compaction:
                write<compaction_metadata>(out, s.contents[val.key]);
                break;
            case metadata_type::Stats:
                write<stats_metadata>(out, s.contents[val.key]);
                break;
            default:
                sstlog.warn("Invalid metadata type at Statistics file: {} ", int(val.key));
                return; // FIXME: should throw
            }
    }
}

future<> parse(random_access_reader& in, estimated_histogram& eh) {
    auto len = std::make_unique<uint32_t>();

    auto f = parse(in, *len);
    return f.then([&in, &eh, len = std::move(len)] {
        uint32_t length = *len;

        assert(length > 0);
        eh.bucket_offsets.resize(length - 1);
        eh.buckets.resize(length);

        auto type_size = sizeof(uint64_t) * 2;
        return in.read_exactly(length * type_size).then([&eh, length, type_size] (auto buf) {
            check_buf_size(buf, length * type_size);

            auto *nr = reinterpret_cast<const net::packed<uint64_t> *>(buf.get());
            size_t j = 0;
            for (size_t i = 0; i < length; ++i) {
                eh.bucket_offsets[i == 0 ? 0 : i - 1] = net::ntoh(nr[j++]);
                eh.buckets[i] = net::ntoh(nr[j++]);
            }
            return make_ready_future<>();
        });
    });
}

inline void write(file_writer& out, estimated_histogram& eh) {
    uint32_t len = 0;
    check_truncate_and_assign(len, eh.buckets.size());

    write(out, len);
    struct element {
        uint64_t offsets;
        uint64_t buckets;
    };
    std::vector<element> elements;
    elements.resize(eh.buckets.size());

    auto *offsets_nr = reinterpret_cast<const net::packed<uint64_t> *>(eh.bucket_offsets.data());
    auto *buckets_nr = reinterpret_cast<const net::packed<uint64_t> *>(eh.buckets.data());
    for (size_t i = 0; i < eh.buckets.size(); i++) {
        elements[i].offsets = net::hton(offsets_nr[i == 0 ? 0 : i - 1]);
        elements[i].buckets = net::hton(buckets_nr[i]);
    }

    auto p = reinterpret_cast<const char*>(elements.data());
    auto bytes = elements.size() * sizeof(element);
    out.write(p, bytes).get();
}

// This is small enough, and well-defined. Easier to just read it all
// at once
future<> sstable::read_toc() {
    auto file_path = filename(sstable::component_type::TOC);

    sstlog.debug("Reading TOC file {} ", file_path);

    return engine().open_file_dma(file_path, open_flags::ro).then([this] (file f) {
        auto bufptr = allocate_aligned_buffer<char>(4096, 4096);
        auto buf = bufptr.get();

        auto fut = f.dma_read(0, buf, 4096);
        return std::move(fut).then([this, f = std::move(f), bufptr = std::move(bufptr)] (size_t size) {
            // This file is supposed to be very small. Theoretically we should check its size,
            // but if we so much as read a whole page from it, there is definitely something fishy
            // going on - and this simplifies the code.
            if (size >= 4096) {
                throw malformed_sstable_exception("SSTable too big: " + to_sstring(size) + " bytes.");
            }

            std::experimental::string_view buf(bufptr.get(), size);
            std::vector<sstring> comps;

            boost::split(comps , buf, boost::is_any_of("\n"));

            for (auto& c: comps) {
                // accept trailing newlines
                if (c == "") {
                    continue;
                }
                try {
                   _components.insert(reverse_map(c, _component_map));
                } catch (std::out_of_range& oor) {
                    throw malformed_sstable_exception("Unrecognized TOC component: " + c);
                }
            }
            if (!_components.size()) {
                throw malformed_sstable_exception("Empty TOC");
            }
            return make_ready_future<>();
        });
    }).then_wrapped([file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
        }
    });

}

future<> sstable::write_toc() {
    auto file_path = filename(sstable::component_type::TOC);

    sstlog.debug("Writing TOC file {} ", file_path);

    return engine().open_file_dma(file_path, open_flags::wo | open_flags::create | open_flags::truncate).then([this] (file f) {
        auto out = file_writer(make_lw_shared<file>(std::move(f)), 4096);
        auto w = make_shared<file_writer>(std::move(out));

        return do_for_each(_components, [this, w] (auto key) {
            // new line character is appended to the end of each component name.
            auto value = _component_map[key] + "\n";
            bytes b = bytes(reinterpret_cast<const bytes::value_type *>(value.c_str()), value.size());
            return seastar::async([w, b = std::move(b)] () mutable { write(*w, b); });
        }).then([w] {
            return w->flush().then([w] {
                return w->close().then([w] {});
            });
        });
    });
}

future<> write_crc(const sstring file_path, checksum& c) {
    sstlog.debug("Writing CRC file {} ", file_path);

    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    return engine().open_file_dma(file_path, oflags).then([&c] (file f) {
        auto out = file_writer(make_lw_shared<file>(std::move(f)), 4096);
        auto w = make_shared<file_writer>(std::move(out));

        return seastar::async([w, &c] () { write(*w, c); }).then([w] {
            return w->close().then([w] {});
        });
    });
}

// Digest file stores the full checksum of data file converted into a string.
future<> write_digest(const sstring file_path, uint32_t full_checksum) {
    sstlog.debug("Writing Digest file {} ", file_path);

    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    return engine().open_file_dma(file_path, oflags).then([full_checksum] (file f) {
        auto out = file_writer(make_lw_shared<file>(std::move(f)), 4096);
        auto w = make_shared<file_writer>(std::move(out));

        return do_with(to_sstring<bytes>(full_checksum), [w] (bytes& digest) {
            return seastar::async([w, &digest] { write(*w, digest); }).then([w] {
                return w->close().then([w] {});
            });
        });
    });
}

future<index_list> sstable::read_indexes(uint64_t position, uint64_t quantity) {
    struct reader {
        uint64_t count = 0;
        std::vector<index_entry> indexes;
        file_random_access_reader stream;
        reader(lw_shared_ptr<file> f, uint64_t quantity) : stream(f) { indexes.reserve(quantity); }
    };

    auto r = make_lw_shared<reader>(_index_file, quantity);

    r->stream.seek(position);

    auto end = [r, quantity] { return r->count >= quantity; };

    return do_until(end, [this, r] {
        r->indexes.emplace_back();
        auto fut = parse(r->stream, r->indexes.back());
        return std::move(fut).then_wrapped([this, r] (future<> f) mutable {
            try {
               f.get();
               r->count++;
            } catch (bufsize_mismatch_exception &e) {
                // We have optimistically emplaced back one element of the
                // vector. If we have failed to parse, we should remove it
                // so size() gives us the right picture.
                r->indexes.pop_back();

                // FIXME: If the file ends at an index boundary, there is
                // no problem. Essentially, we can't know how many indexes
                // are in a sampling group, so there isn't really any way
                // to know, other than reading.
                //
                // If, however, we end in the middle of an index, this is a
                // corrupted file. This code is not perfect because we only
                // know that an exception happened, and it happened due to
                // eof. We don't really know if eof happened at the index
                // boundary.  To know that, we would have to keep track of
                // the real position of the stream (including what's
                // already in the buffer) before we start to read the
                // index, and after. We won't go through such complexity at
                // the moment.
                if (r->stream.eof()) {
                    r->count = std::numeric_limits<std::remove_reference<decltype(r->count)>::type>::max();
                } else {
                    throw e;
                }
            }
            return make_ready_future<>();
        });
    }).then([r] {
        return make_ready_future<index_list>(std::move(r->indexes));
    });
}

template <sstable::component_type Type, typename T>
future<> sstable::read_simple(T& component) {

    auto file_path = filename(Type);
    sstlog.debug(("Reading " + _component_map[Type] + " file {} ").c_str(), file_path);
    return engine().open_file_dma(file_path, open_flags::ro).then([this, &component] (file f) {

        auto r = std::make_unique<file_random_access_reader>(std::move(f), 4096);
        auto fut = parse(*r, component);
        return fut.then([r = std::move(r)] {});
    }).then_wrapped([this, file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
        }
    });
}

template <sstable::component_type Type, typename T>
future<> sstable::write_simple(T& component) {
    auto file_path = filename(Type);
    sstlog.debug(("Writing " + _component_map[Type] + " file {} ").c_str(), file_path);
    return engine().open_file_dma(file_path, open_flags::wo | open_flags::create | open_flags::truncate).then([this, &component] (file f) {

        auto out = file_writer(make_lw_shared<file>(std::move(f)), 4096);
        auto w = make_shared<file_writer>(std::move(out));
        auto fut = seastar::async([w, &component] () mutable { write(*w, component); });
        return fut.then([w] {
            return w->flush().then([w] {
                return w->close().then([w] {}); // the underlying file is synced here.
            });
        });
    }).then_wrapped([this, file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            // TODO: handle exception.
        }
    });
}
template future<> sstable::read_simple<sstable::component_type::Filter>(sstables::filter& f);
template future<> sstable::write_simple<sstable::component_type::Filter>(sstables::filter& f);

future<> sstable::read_compression() {
     // FIXME: If there is no compression, we should expect a CRC file to be present.
    if (!has_component(sstable::component_type::CompressionInfo)) {
        return make_ready_future<>();
    }

    return read_simple<component_type::CompressionInfo>(_compression);
}

future<> sstable::write_compression() {
    if (!has_component(sstable::component_type::CompressionInfo)) {
        return make_ready_future<>();
    }

    return write_simple<component_type::CompressionInfo>(_compression);
}

future<> sstable::read_statistics() {
    return read_simple<component_type::Statistics>(_statistics);
}

future<> sstable::write_statistics() {
    return write_simple<component_type::Statistics>(_statistics);
}

future<> sstable::open_data() {
    return when_all(engine().open_file_dma(filename(component_type::Index), open_flags::ro),
                    engine().open_file_dma(filename(component_type::Data), open_flags::ro)).then([this] (auto files) {
        _index_file = make_lw_shared<file>(std::move(std::get<file>(std::get<0>(files).get())));
        _data_file  = make_lw_shared<file>(std::move(std::get<file>(std::get<1>(files).get())));
        return _data_file->size().then([this] (auto size) {
          _data_file_size = size;
        });
    });
}

future<> sstable::create_data() {
    auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
    return when_all(engine().open_file_dma(filename(component_type::Index), oflags),
                    engine().open_file_dma(filename(component_type::Data), oflags)).then([this] (auto files) {
        _index_file = make_lw_shared<file>(std::move(std::get<file>(std::get<0>(files).get())));
        _data_file  = make_lw_shared<file>(std::move(std::get<file>(std::get<1>(files).get())));
    });
}

future<> sstable::load() {
    return read_toc().then([this] {
        return read_statistics();
    }).then([this] {
        return read_compression();
    }).then([this] {
        return read_filter();
    }).then([this] {;
        return read_summary();
    }).then([this] {
        return open_data();
    }).then([this] {
        // After we have _compression and _data_file_size, we can update
        // _compression with additional information it needs:
        if (has_component(sstable::component_type::CompressionInfo)) {
            _compression.update(_data_file_size);
        }
    });
}

future<> sstable::store() {
    // TODO: write other components as well.
    return write_toc().then([this] {
        return write_statistics();
    }).then([this] {
        return write_compression();
    }).then([this] {
        return write_filter();
    }).then([this] {
        return write_summary();
    });
}

// @clustering_key: it's expected that clustering key is already in its composite form.
// NOTE: empty clustering key means that there is no clustering key.
void sstable::write_column_name(file_writer& out, const composite& clustering_key, const std::vector<bytes_view>& column_names, composite_marker m) {
    // FIXME: min_components and max_components also keep track of clustering
    // prefix, so we must merge clustering_key and column_names somehow and
    // pass the result to the functions below.
    column_name_helper::min_components(_c_stats.min_column_names, column_names);
    column_name_helper::max_components(_c_stats.max_column_names, column_names);

    // FIXME: This code assumes name is always composite, but it wouldn't if "WITH COMPACT STORAGE"
    // was defined in the schema, for example.
    auto c= composite::from_exploded(column_names, m);
    auto ck_bview = bytes_view(clustering_key);

    // The marker is not a component, so if the last component is empty (IOW,
    // only serializes to the marker), then we just replace the key's last byte
    // with the marker. If the component however it is not empty, then the
    // marker should be in the end of it, and we just join them together as we
    // do for any normal component
    if (c.size() == 1) {
        ck_bview.remove_suffix(1);
    }
    uint16_t sz = ck_bview.size() + c.size();
    write(out, sz, ck_bview, c);
}

static inline void update_cell_stats(column_stats& c_stats, uint64_t timestamp) {
    c_stats.update_min_timestamp(timestamp);
    c_stats.update_max_timestamp(timestamp);
    c_stats.column_count++;
}

// Intended to write all cell components that follow column name.
void sstable::write_cell(file_writer& out, atomic_cell_view cell) {
    // FIXME: range tombstone and counter cells aren't supported yet.

    uint64_t timestamp = cell.timestamp();

    update_cell_stats(_c_stats, timestamp);

    if (cell.is_live_and_has_ttl()) {
        // expiring cell

        column_mask mask = column_mask::expiration;
        uint32_t ttl = cell.ttl().count();
        uint32_t expiration = cell.expiry().time_since_epoch().count();
        disk_string_view<uint32_t> cell_value { cell.value() };

        write(out, mask, ttl, expiration, timestamp, cell_value);
    } else if (cell.is_dead()) {
        // tombstone cell

        column_mask mask = column_mask::deletion;
        uint32_t deletion_time_size = sizeof(uint32_t);
        uint32_t deletion_time = cell.deletion_time().time_since_epoch().count();

        _c_stats.tombstone_histogram.update(deletion_time);

        write(out, mask, timestamp, deletion_time_size, deletion_time);
    } else {
        // regular cell

        column_mask mask = column_mask::none;
        disk_string_view<uint32_t> cell_value { cell.value() };

        write(out, mask, timestamp, cell_value);
    }
}

void sstable::write_row_marker(file_writer& out, const rows_entry& clustered_row, const composite& clustering_key) {
    // Missing created_at (api::missing_timestamp) means no row marker.
    if (clustered_row.row().created_at() == api::missing_timestamp) {
        return;
    }

    // Write row mark cell to the beginning of clustered row.
    write_column_name(out, clustering_key, { bytes_view() });
    column_mask mask = column_mask::none;
    uint64_t timestamp = clustered_row.row().created_at();
    uint32_t value_length = 0;

    update_cell_stats(_c_stats, timestamp);

    write(out, mask, timestamp, value_length);
}

void sstable::write_range_tombstone(file_writer& out, const composite& clustering_prefix, std::vector<bytes_view> suffix, const tombstone t) {
    if (!t) {
        return;
    }

    write_column_name(out, clustering_prefix, suffix, composite_marker::start_range);
    column_mask mask = column_mask::range_tombstone;
    write(out, mask);
    write_column_name(out, clustering_prefix, suffix, composite_marker::end_range);
    uint64_t timestamp = t.timestamp;
    uint32_t deletion_time = t.deletion_time.time_since_epoch().count();

    write(out, deletion_time, timestamp);
}

void sstable::write_collection(file_writer& out, const composite& clustering_key, const column_definition& cdef, collection_mutation::view collection) {

    auto t = static_pointer_cast<const collection_type_impl>(cdef.type);
    auto mview = t->deserialize_mutation_form(collection);
    const bytes& column_name = cdef.name();
    write_range_tombstone(out, clustering_key, { bytes_view(column_name) }, mview.tomb);
    for (auto& cp: mview.cells) {
        write_column_name(out, clustering_key, { column_name, cp.first });
        write_cell(out, cp.second);
    }
}

// write_datafile_clustered_row() is about writing a clustered_row to data file according to SSTables format.
// clustered_row contains a set of cells sharing the same clustering key.
void sstable::write_clustered_row(file_writer& out, const schema& schema, const rows_entry& clustered_row) {
    auto clustering_key = composite::from_clustering_element(schema, clustered_row.key());

    write_row_marker(out, clustered_row, clustering_key);
    // FIXME: Before writing cells, range tombstone must be written if the row has any (deletable_row::t).
    assert(!clustered_row.row().deleted_at());

    // Write all cells of a partition's row.
    for (auto& value: clustered_row.row().cells()) {
        auto column_id = value.first;
        auto&& column_definition = schema.regular_column_at(column_id);
        // non atomic cell isn't supported yet. atomic cell maps to a single trift cell.
        // non atomic cell maps to multiple trift cell, e.g. collection.
        if (!column_definition.is_atomic()) {
            write_collection(out, clustering_key, column_definition, value.second.as_collection_mutation());
            return;
        }
        assert(column_definition.is_regular());
        atomic_cell_view cell = value.second.as_atomic_cell();
        const bytes& column_name = column_definition.name();

        write_column_name(out, clustering_key, { bytes_view(column_name) });
        write_cell(out, cell);
    }
}

void sstable::write_static_row(file_writer& out, const schema& schema, const row& static_row) {
    for (auto& value: static_row) {
        auto column_id = value.first;
        auto&& column_definition = schema.static_column_at(column_id);
        if (!column_definition.is_atomic()) {
            auto sp = composite::static_prefix(schema);
            write_collection(out, sp, column_definition, value.second.as_collection_mutation());
            return;
        }
        assert(column_definition.is_static());
        atomic_cell_view cell = value.second.as_atomic_cell();
        auto sp = composite::static_prefix(schema);
        write_column_name(out, sp, { bytes_view(column_definition.name()) });
        write_cell(out, cell);
    }
}

static void write_index_entry(file_writer& out, disk_string_view<uint16_t>& key, uint64_t pos) {
    // FIXME: support promoted indexes.
    uint32_t promoted_index_size = 0;

    write(out, key, pos, promoted_index_size);
}

static constexpr int BASE_SAMPLING_LEVEL = 128;

static void prepare_summary(summary& s, const memtable& mt) {
    auto&& all_partitions = mt.all_partitions();
    assert(all_partitions.size() >= 1);

    s.header.min_index_interval = BASE_SAMPLING_LEVEL;
    s.header.sampling_level = BASE_SAMPLING_LEVEL;

    uint64_t max_expected_entries = all_partitions.size() / BASE_SAMPLING_LEVEL + 1;
    // FIXME: handle case where max_expected_entries is greater than max value stored by uint32_t.
    assert(max_expected_entries <= std::numeric_limits<uint32_t>::max());
    s.header.size = max_expected_entries;
    assert(s.header.size >= 1);

    // memory_size only accounts size of vector positions at this point.
    s.header.memory_size = s.header.size * sizeof(uint32_t);
    s.header.size_at_full_sampling = s.header.size;

    s.positions.reserve(s.header.size);
    s.entries.reserve(s.header.size);
    s.keys_written = 0;

    auto begin = all_partitions.begin();
    auto last = --all_partitions.end();

    auto first_key = key::from_partition_key(*mt.schema(), begin->first._key);
    s.first_key.value = std::move(first_key.get_bytes());

    auto last_key = key::from_partition_key(*mt.schema(), last->first._key);
    s.last_key.value = std::move(last_key.get_bytes());
}

// In the beginning of the statistics file, there is a disk_hash used to
// describe where each type of metadata lies. Key is the metadata_type, and
// Value the offset.
// The purpose of this function is to initialize the field offset, used to
// help the computation of the offset value for each metadata type.
static void prepare_statistics(statistics& s) {
    // FIXME: When compaction metadata is supported, METADATA_TYPE_COUNT must be 3 instead.
    static constexpr int METADATA_TYPE_COUNT = 2;

    s.offset = 0;
    // account disk_hash size.
    s.offset += sizeof(uint32_t);
    // account disk_hash members.
    s.offset += (METADATA_TYPE_COUNT * (sizeof(metadata_type) + sizeof(uint32_t)));
}

static void prepare_compression(compression& c, const schema& schema) {
    c.set_compressor(schema.get_compressor());
    // FIXME: chunk length can be configured by the user.
    c.chunk_len = DEFAULT_CHUNK_SIZE;
    c.data_len = 0;
    // probability to verify the checksum of a compressed chunk we read.
    // defaults to 1.0.
    c.options.elements.push_back({"crc_check_chance", "1.0"});
    c.init_full_checksum();
}

static void maybe_add_summary_entry(summary& s, bytes_view key, uint64_t offset) {
    if ((s.keys_written % s.header.min_index_interval) == 0) {
        s.positions.push_back(s.header.memory_size);
        s.entries.push_back({ bytes(key.data(), key.size()), offset });
        s.header.memory_size += key.size() + sizeof(uint64_t);
    }
    s.keys_written++;
}

static void add_validation_metadata(statistics& s, const bytes partitioner, double bloom_filter_fp_chance)
{
    size_t old_offset = s.offset;
    validation_metadata m;

    m.partitioner.value = partitioner;
    m.filter_chance = bloom_filter_fp_chance;
    s.offset += m.serialized_size();
    s.contents[metadata_type::Validation] = std::make_unique<validation_metadata>(std::move(m));
    s.hash.map[metadata_type::Validation] = old_offset;
}

static void add_stats_metadata(statistics& s, metadata_collector& collector) {
    stats_metadata m;
    collector.construct_stats(m);
    s.contents[metadata_type::Stats] = std::make_unique<stats_metadata>(std::move(m));
    s.hash.map[metadata_type::Stats] = s.offset;
    // FIXME: method serialized_size of stats_metadata must be implemented for
    // compaction_metadata to get supported, then increment s.offset using it.
}

static constexpr size_t sstable_buffer_size = 64*1024;

///
///  @param out holds an output stream to data file.
///
void sstable::do_write_components(const memtable& mt, file_writer& out) {
    auto index = make_shared<file_writer>(_index_file, sstable_buffer_size);

    prepare_summary(_summary, mt);
    auto filter_fp_chance = mt.schema()->bloom_filter_fp_chance();
    if (filter_fp_chance != 1.0) {
        _components.insert(component_type::Filter);
    }
    _filter = utils::i_filter::get_filter(mt.all_partitions().size(), filter_fp_chance);

    prepare_statistics(_statistics);

    // NOTE: Cassandra gets partition name by calling getClass().getCanonicalName() on
    // partition class.
    add_validation_metadata(_statistics, dht::global_partitioner().name(), filter_fp_chance);
    auto collector = make_lw_shared<metadata_collector>();

    // Iterate through CQL partitions, then CQL rows, then CQL columns.
    // Each mt.all_partitions() entry is a set of clustered rows sharing the same partition key.
    for (auto& partition_entry: mt.all_partitions()) {
        // FIXME: it's likely that we need to set both sstable_level and repaired_at at this point.
        // Set current index of data to later compute row size.
        _c_stats.start_offset = out.offset();
        auto partition_key = key::from_partition_key(*mt.schema(), partition_entry.first._key);
        // Maybe add summary entry into in-memory representation of summary file.
        maybe_add_summary_entry(_summary, bytes_view(partition_key), index->offset());
        _filter->add(bytes_view(partition_key));

        auto p_key = disk_string_view<uint16_t>();
        p_key.value = bytes_view(partition_key);
        // Write index file entry from partition key into index file.

        write_index_entry(*index, p_key, out.offset());

        // Write partition key into data file.
        write(out, p_key);

        auto tombstone = partition_entry.second.partition_tombstone();
        deletion_time d;

        if (tombstone) {
            d.local_deletion_time = tombstone.deletion_time.time_since_epoch().count();
            d.marked_for_delete_at = tombstone.timestamp;

            _c_stats.tombstone_histogram.update(d.local_deletion_time);
            _c_stats.update_max_local_deletion_time(d.local_deletion_time);
            _c_stats.update_min_timestamp(d.marked_for_delete_at);
            _c_stats.update_max_timestamp(d.marked_for_delete_at);
        } else {
            // Default values for live, undeleted rows.
            d.local_deletion_time = std::numeric_limits<int32_t>::max();
            d.marked_for_delete_at = std::numeric_limits<int64_t>::min();
        }
        write(out, d);

        auto& partition = partition_entry.second;
        auto& static_row = partition.static_row();

        write_static_row(out, *mt.schema(), static_row);
        for (const auto& rt: partition.row_tombstones()) {
            auto prefix = composite::from_clustering_element(*mt.schema(), rt.prefix());
            write_range_tombstone(out, prefix, {}, rt.t());
        }

        // Write all CQL rows from a given mutation partition.
        for (auto& clustered_row: partition.clustered_rows()) {
            write_clustered_row(out, *mt.schema(), clustered_row);
        }
        int16_t end_of_row = 0;
        write(out, end_of_row);

        // compute size of the current row.
        _c_stats.row_size = out.offset() - _c_stats.start_offset;
        // update is about merging column_stats with the data being stored by collector.
        collector->update(_c_stats);
        _c_stats.reset();
    }

    index->close().get();

    _components.insert(component_type::TOC);
    _components.insert(component_type::Statistics);
    _components.insert(component_type::Digest);
    _components.insert(component_type::Index);
    _components.insert(component_type::Summary);
    _components.insert(component_type::Data);

    add_stats_metadata(_statistics, *collector);
}

void sstable::prepare_write_components(const memtable& mt) {
    // CRC component must only be present when compression isn't enabled.
    bool checksum_file = mt.schema()->get_compressor() == compressor::none;

    if (checksum_file) {
        auto w = make_shared<checksummed_file_writer>(_data_file, sstable_buffer_size, checksum_file);
        _components.insert(component_type::CRC);
        this->do_write_components(mt, *w);
        w->close().get();

        write_digest(filename(sstable::component_type::Digest), w->full_checksum()).get();
        write_crc(filename(sstable::component_type::CRC), w->finalize_checksum()).get();
    } else {
        prepare_compression(_compression, *mt.schema());
        auto w = make_shared<file_writer>(make_compressed_file_output_stream(_data_file, &_compression));
        _components.insert(component_type::CompressionInfo);
        this->do_write_components(mt, *w);
        w->close().get();

        write_digest(filename(sstable::component_type::Digest), _compression.full_checksum()).get();
    }
}

future<> sstable::write_components(const memtable& mt) {
    return create_data().then([this, &mt] {
        auto w = [this] (const memtable& mt) {
            this->prepare_write_components(mt);
        };
        return seastar::async(w, mt).then([this] {
            return write_summary();
        }).then([this] {
            return write_filter();
        }).then([this] {
            return write_statistics();
        }).then([this] {
            // NOTE: write_compression means maybe_write_compression.
            return write_compression();
        }).then([this] {
            return write_toc();
        });
    });
}

size_t sstable::data_size() {
    if (has_component(sstable::component_type::CompressionInfo)) {
        return _compression.data_len;
    }
    return _data_file_size;
}

const bool sstable::has_component(component_type f) {
    return _components.count(f);
}

const sstring sstable::filename(component_type f) {

    auto& version = _version_string.at(_version);
    auto& format = _format_string.at(_format);
    auto& component = _component_map.at(f);
    auto generation =  to_sstring(_generation);

    return _dir + "/" + version + "-" + generation + "-" + format + "-" + component;
}

const sstring sstable::filename(sstring dir, version_types version, unsigned long generation,
                                format_types format, component_type component) {
    auto& v = _version_string.at(version);
    auto& f = _format_string.at(format);
    auto& c= _component_map.at(component);
    auto g =  to_sstring(generation);

    return dir + "/" + v + "-" + g + "-" + f + "-" + c;
}

sstable::version_types sstable::version_from_sstring(sstring &s) {
    return reverse_map(s, _version_string);
}

sstable::format_types sstable::format_from_sstring(sstring &s) {
    return reverse_map(s, _format_string);
}

input_stream<char> sstable::data_stream_at(uint64_t pos) {
    if (_compression) {
        return make_compressed_file_input_stream(
                _data_file, &_compression, pos);
    } else {
        return make_file_input_stream(_data_file, pos);
    }
}

// FIXME: to read a specific byte range, we shouldn't use the input stream
// interface - it may cause too much read when we intend to read a small
// range, and too small reads, and repeated waits, when reading a large range
// which we should have started at once.
future<temporary_buffer<char>> sstable::data_read(uint64_t pos, size_t len) {
    return do_with(data_stream_at(pos), [len] (auto& stream) {
        return stream.read_exactly(len);
    });
}

}
