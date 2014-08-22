// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/serialize_datum.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "containers/archive/buffer_stream.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/counted.hpp"
#include "containers/shared_buffer.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"

namespace ql {


enum class datum_serialized_type_t {
    R_ARRAY = 1,
    R_BOOL = 2,
    R_NULL = 3,
    DOUBLE = 4,
    R_OBJECT = 5,
    R_STR = 6,
    INT_NEGATIVE = 7,
    INT_POSITIVE = 8,
    R_BINARY = 9,
    BUF_R_ARRAY = 10,
    BUF_R_OBJECT = 11
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(datum_serialized_type_t, int8_t,
                                      datum_serialized_type_t::R_ARRAY,
                                      datum_serialized_type_t::BUF_R_OBJECT);

serialization_result_t datum_serialize(write_message_t *wm,
                                       datum_serialized_type_t type) {
    serialize<cluster_version_t::LATEST_OVERALL>(wm, type);
    return serialization_result_t::SUCCESS;
}

MUST_USE archive_result_t datum_deserialize(read_stream_t *s,
                                            datum_serialized_type_t *type) {
    return deserialize<cluster_version_t::LATEST_OVERALL>(s, type);
}

// This looks like it duplicates code of other deserialization functions.  It does.
// Keeping this separate means that we don't have to worry about whether datum
// serialization has changed from cluster version to cluster version.

// Keep in sync with datum_array_serialize.
size_t datum_array_inner_serialized_size(const datum_t &datum,
                                         std::vector<size_t> *element_sizes_out) {
    if (element_sizes_out != NULL) {
        rassert(element_sizes_out->empty());
        element_sizes_out->reserve(datum.size());
    }

    size_t sz = 0;

    // The size of num_elements
    sz += varint_uint64_serialized_size(datum.size());

    // The size of the offset table
    if (datum.size() > 1) {
        sz += (datum.size() - 1) * serialize_universal_size_t<uint32_t>::value;
    }

    // The size of all elements
    for (size_t i = 0; i < datum.size(); ++i) {
        auto elem = datum.get(i);
        const size_t elem_size = datum_serialized_size(elem);
        if (element_sizes_out != NULL) {
            element_sizes_out->push_back(elem_size);
        }
        sz += elem_size;
    }

    return sz;
}
size_t datum_array_serialized_size(const datum_t &datum) {
    size_t sz = datum_array_inner_serialized_size(datum, NULL);

    // The inner serialized size
    sz += varint_uint64_serialized_size(sz);

    return sz;
}


// Keep in sync with datum_array_serialized_size.
// Keep in sync with datum_get_element_offset.
// Keep in sync with datum_get_array_size.
serialization_result_t datum_array_serialize(write_message_t *wm,
                                             const datum_t &datum) {
    serialization_result_t res = serialization_result_t::SUCCESS;

    // The inner serialized size
    std::vector<size_t> element_sizes;
    serialize_varint_uint64(wm, datum_array_inner_serialized_size(datum, &element_sizes));

    // num_elements
    serialize_varint_uint64(wm, datum.size());

    // The offset table
    size_t next_element_offset = 0;
    for (size_t i = 1; i < element_sizes.size(); ++i) {
        next_element_offset += element_sizes[i-1];
        guarantee(next_element_offset <= std::numeric_limits<uint32_t>::max(),
                  "Array is too large for serialization (> 4 GB)");
        serialize_universal(wm, static_cast<uint32_t>(next_element_offset));
    }

    // The elements
    for (size_t i = 0; i < datum.size(); ++i) {
        auto elem = datum.get(i);
        res = res | datum_serialize(wm, elem);
    }

    return res;
}

// For legacy R_ARRAY datums. BUF_R_ARRAY datums are not deserialized through this.
MUST_USE archive_result_t
datum_deserialize(read_stream_t *s, std::vector<datum_t> *v) {
    v->clear();

    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (bad(res)) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    v->resize(sz);
    for (uint64_t i = 0; i < sz; ++i) {
        res = datum_deserialize(s, &(*v)[i]);
        if (bad(res)) { return res; }
    }

    return archive_result_t::SUCCESS;
}

// Keep in sync with datum_object_serialize
size_t datum_object_inner_serialized_size(const datum_t &datum,
                                          std::vector<size_t> *pair_sizes_out) {
    if (pair_sizes_out != NULL) {
        rassert(pair_sizes_out->empty());
        pair_sizes_out->reserve(datum.num_pairs());
    }

    size_t sz = 0;

    // The size of num_elements
    sz += varint_uint64_serialized_size(datum.num_pairs());

    // The size of the offset table
    if (datum.num_pairs() > 1) {
        sz += (datum.num_pairs() - 1) * serialize_universal_size_t<uint32_t>::value;
    }

    // The size of all pairs
    for (size_t i = 0; i < datum.num_pairs(); ++i) {
        auto pair = datum.get_pair(i);
        const size_t pair_size = datum_serialized_size(pair.first)
                                 + datum_serialized_size(pair.second);
        if (pair_sizes_out != NULL) {
            pair_sizes_out->push_back(pair_size);
        }
        sz += pair_size;
    }

    return sz;
}
size_t datum_object_serialized_size(const datum_t &datum) {
    size_t sz = datum_object_inner_serialized_size(datum, NULL);

    // The inner serialized size
    sz += varint_uint64_serialized_size(sz);

    return sz;
}

// Keep in sync with datum_object_serialized_size.
// Keep in sync with datum_get_element_offset.
// Keep in sync with datum_get_array_size.
// Keep in sync with datum_deserialize_pair_from_buf.
serialization_result_t datum_object_serialize(write_message_t *wm,
                                              const datum_t &datum) {
    serialization_result_t res = serialization_result_t::SUCCESS;

    // The inner serialized size
    std::vector<size_t> pair_sizes;
    serialize_varint_uint64(wm, datum_object_inner_serialized_size(datum, &pair_sizes));

    // num_elements
    serialize_varint_uint64(wm, datum.num_pairs());

    // The offset table
    size_t next_pair_offset = 0;
    for (size_t i = 1; i < pair_sizes.size(); ++i) {
        next_pair_offset += pair_sizes[i-1];
        guarantee(next_pair_offset <= std::numeric_limits<uint32_t>::max(),
                  "Object is too large for serialization (> 4 GB)");
        serialize_universal(wm, static_cast<uint32_t>(next_pair_offset));
    }

    // The pairs
    for (size_t i = 0; i < datum.num_pairs(); ++i) {
        auto pair = datum.get_pair(i);
        res = res | datum_serialize(wm, pair.first);
        res = res | datum_serialize(wm, pair.second);
    }

    return res;
}

// For legacy R_OBJECT datums. BUF_R_OBJECT datums are not deserialized through this.
MUST_USE archive_result_t datum_deserialize(
        read_stream_t *s,
        std::vector<std::pair<datum_string_t, datum_t> > *m) {
    m->clear();

    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (bad(res)) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    m->reserve(static_cast<size_t>(sz));

    for (uint64_t i = 0; i < sz; ++i) {
        std::pair<datum_string_t, datum_t> p;
        res = datum_deserialize(s, &p.first);
        if (bad(res)) { return res; }
        res = datum_deserialize(s, &p.second);
        if (bad(res)) { return res; }
        m->push_back(std::move(p));
    }

    return archive_result_t::SUCCESS;
}




size_t datum_serialized_size(const datum_t &datum) {
    r_sanity_check(datum.has());
    size_t sz = 1; // 1 byte for the type
    switch (datum.get_type()) {
    case datum_t::R_ARRAY: {
        sz += datum_array_serialized_size(datum);
    } break;
    case datum_t::R_BINARY: {
        sz += datum_serialized_size(datum.as_binary());
    } break;
    case datum_t::R_BOOL: {
        sz += serialize_universal_size_t<bool>::value;
    } break;
    case datum_t::R_NULL: break;
    case datum_t::R_NUM: {
        double d = datum.as_num();
        int64_t i;
        if (number_as_integer(d, &i)) {
            sz += varint_uint64_serialized_size(i < 0 ? -i : i);
        } else {
            sz += serialize_universal_size_t<double>::value;
        }
    } break;
    case datum_t::R_OBJECT: {
        sz += datum_object_serialized_size(datum);
    } break;
    case datum_t::R_STR: {
        sz += datum_serialized_size(datum.as_str());
    } break;
    case datum_t::UNINITIALIZED: // fallthru
    default:
        unreachable();
    }
    return sz;
}
serialization_result_t datum_serialize(write_message_t *wm,
                                       const datum_t &datum) {
    serialization_result_t res = serialization_result_t::SUCCESS;
    r_sanity_check(datum.has());
    switch (datum->get_type()) {
    case datum_t::R_ARRAY: {
        res = res | datum_serialize(wm, datum_serialized_type_t::BUF_R_ARRAY);
        if (datum.size() > 100000)
            res = res | serialization_result_t::ARRAY_TOO_BIG;
        res = res | datum_array_serialize(wm, datum);
    } break;
    case datum_t::R_BINARY: {
        datum_serialize(wm, datum_serialized_type_t::R_BINARY);
        const datum_string_t &value = datum->as_binary();
        datum_serialize(wm, value);
    } break;
    case datum_t::R_BOOL: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_BOOL);
        bool value = datum->as_bool();
        serialize_universal(wm, value);
    } break;
    case datum_t::R_NULL: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_NULL);
    } break;
    case datum_t::R_NUM: {
        double value = datum->as_num();
        int64_t i;
        if (number_as_integer(value, &i)) {
            // We serialize the signed-zero double, -0.0, with INT_NEGATIVE.

            // so we can use `signbit` in a GCC 4.4.3-compatible way
            using namespace std;  // NOLINT(build/namespaces)
            if (signbit(value)) {
                res = res | datum_serialize(wm, datum_serialized_type_t::INT_NEGATIVE);
                serialize_varint_uint64(wm, -i);
            } else {
                res = res | datum_serialize(wm, datum_serialized_type_t::INT_POSITIVE);
                serialize_varint_uint64(wm, i);
            }
        } else {
            res = res | datum_serialize(wm, datum_serialized_type_t::DOUBLE);
            serialize_universal(wm, value);
        }
    } break;
    case datum_t::R_OBJECT: {
        res = res | datum_serialize(wm, datum_serialized_type_t::BUF_R_OBJECT);
        res = res | datum_object_serialize(wm, datum);
    } break;
    case datum_t::R_STR: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_STR);
        const datum_string_t &value = datum->as_str();
        res = res | datum_serialize(wm, value);
    } break;
    case datum_t::UNINITIALIZED: // fallthru
    default:
        unreachable();
    }
    return res;
}
archive_result_t datum_deserialize(read_stream_t *s, datum_t *datum) {
    // Datums on disk should always be read no matter how stupid big
    // they are; there's no way to fix the problem otherwise.
    // Similarly we don't want to reject array reads from cluster
    // nodes that are within the user spec but larger than the default
    // 100,000 limit.
    ql::configured_limits_t limits = ql::configured_limits_t::unlimited;
    datum_serialized_type_t type;
    archive_result_t res = datum_deserialize(s, &type);
    if (bad(res)) {
        return res;
    }

    switch (type) {
    case datum_serialized_type_t::R_ARRAY: {
        std::vector<datum_t> value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t(std::move(value), limits);
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::R_BINARY: {
        datum_string_t value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t::binary(std::move(value));
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::R_BOOL: {
        bool value;
        res = deserialize_universal(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t::boolean(value);
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::R_NULL: {
        *datum = datum_t::null();
    } break;
    case datum_serialized_type_t::DOUBLE: {
        double value;
        res = deserialize_universal(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t(value);
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::INT_NEGATIVE:  // fall through
    case datum_serialized_type_t::INT_POSITIVE: {
        uint64_t unsigned_value;
        res = deserialize_varint_uint64(s, &unsigned_value);
        if (bad(res)) {
            return res;
        }
        if (unsigned_value > max_dbl_int) {
            return archive_result_t::RANGE_ERROR;
        }
        const double d = unsigned_value;
        double value;
        if (type == datum_serialized_type_t::INT_NEGATIVE) {
            // This might deserialize the signed-zero double, -0.0.
            value = -d;
        } else {
            value = d;
        }
        try {
            *datum = datum_t(value);
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::R_OBJECT: {
        std::vector<std::pair<datum_string_t, datum_t> > value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t(std::move(value));
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::R_STR: {
        datum_string_t value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        try {
            *datum = datum_t(std::move(value));
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    case datum_serialized_type_t::BUF_R_ARRAY: // fallthru
    case datum_serialized_type_t::BUF_R_OBJECT:
    {
        // First read the serialized size of the buffer
        uint64_t ser_size;
        res = deserialize_varint_uint64(s, &ser_size);
        if (bad(res)) {
            return res;
        }
        if (ser_size > std::numeric_limits<size_t>::max()
            || ser_size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            return archive_result_t::RANGE_ERROR;
        }

        // Then read the data into a shared_buf_t
        counted_t<shared_buf_t> buf = shared_buf_t::create(static_cast<size_t>(ser_size));
        int64_t num_read = force_read(s, buf->data(), ser_size);
        if (num_read == -1) {
            return archive_result_t::SOCK_ERROR;
        }
        if (static_cast<uint64_t>(num_read) < ser_size) {
            return archive_result_t::SOCK_EOF;
        }

        // ...from which we create the datum_t
        datum_t::type_t dtype = type == datum_serialized_type_t::BUF_R_ARRAY
                                ? datum_t::R_ARRAY
                                : datum_t::R_OBJECT;
        try {
            *datum = datum_t(dtype, shared_buf_ref_t<char>(std::move(buf), 0));
        } catch (const base_exc_t &) {
            return archive_result_t::RANGE_ERROR;
        }
    } break;
    default:
        return archive_result_t::RANGE_ERROR;
    }

    return archive_result_t::SUCCESS;
}

datum_t datum_deserialize_from_buf(const shared_buf_ref_t<char> &buf, size_t at_offset) {
    // Peak into the buffer to find out the type of the datum in there.
    // If it's a string, buf_object or buf_array, we just create a datum from a
    // child buf_ref and are done.
    // Otherwise we create a buffer_read_stream_t and deserialize the datum from
    // there.
    buf.guarantee_in_boundary(at_offset);
    buffer_read_stream_t read_stream(buf.get() + at_offset,
                                     buf.get_safety_boundary() - at_offset);
    datum_serialized_type_t type = datum_serialized_type_t::R_NULL;
    guarantee_deserialization(datum_deserialize(&read_stream, &type), "datum type from buf");

    switch (type) {
    case datum_serialized_type_t::R_STR: {
        const size_t data_offset = at_offset + static_cast<size_t>(read_stream.tell());
        return datum_t(datum_string_t(buf.make_child(data_offset)));
    }
    case datum_serialized_type_t::BUF_R_ARRAY: {
        // Skip the serialized size that's stored at the front of the array
        uint64_t ser_size;
        guarantee_deserialization(deserialize_varint_uint64(&read_stream, &ser_size),
                                  "datum serialized size from buf");
        const size_t data_offset = at_offset + static_cast<size_t>(read_stream.tell());
        return datum_t(datum_t::R_ARRAY, buf.make_child(data_offset));
    }
    case datum_serialized_type_t::BUF_R_OBJECT: {
        // Skip the serialized size that's stored at the front of the object
        uint64_t ser_size;
        guarantee_deserialization(deserialize_varint_uint64(&read_stream, &ser_size),
                                  "datum serialized size from buf");
        const size_t data_offset = at_offset + static_cast<size_t>(read_stream.tell());
        return datum_t(datum_t::R_OBJECT, buf.make_child(data_offset));
    }
    case datum_serialized_type_t::R_BINARY: {
        const size_t data_offset = at_offset + static_cast<size_t>(read_stream.tell());
        return datum_t(datum_t::construct_binary_t(),
                       datum_string_t(buf.make_child(data_offset)));
    }
    case datum_serialized_type_t::R_ARRAY: // fallthru
    case datum_serialized_type_t::R_BOOL: // fallthru
    case datum_serialized_type_t::R_NULL: // fallthru
    case datum_serialized_type_t::DOUBLE: // fallthru
    case datum_serialized_type_t::R_OBJECT: // fallthru
    case datum_serialized_type_t::INT_NEGATIVE: // fallthru
    case datum_serialized_type_t::INT_POSITIVE: {
        buffer_read_stream_t data_read_stream(buf.get() + at_offset,
                                              buf.get_safety_boundary() - at_offset);
        datum_t res;
        guarantee_deserialization(datum_deserialize(&data_read_stream, &res),
                                  "datum from buf");
        return res;
    }
    default:
        unreachable();
    }
}

std::pair<datum_string_t, datum_t> datum_deserialize_pair_from_buf(
        const shared_buf_ref_t<char> &buf, size_t at_offset) {
    datum_string_t key(buf.make_child(at_offset));
    // Relies on the fact that the datum_string_t serialization format hasn't
    // changed, specifically that we would still get the same size if we re-serialized
    // the datum_string_t now.
    const size_t key_ser_size = datum_serialized_size(key);

    datum_t value(datum_deserialize_from_buf(buf, at_offset + key_ser_size));

    return std::make_pair(std::move(key), std::move(value));
}

/* The format of `array` is:
     varint num_elements
     ... */
size_t datum_get_array_size(const shared_buf_ref_t<char> &array) {
    buffer_read_stream_t sz_read_stream(array.get(), array.get_safety_boundary());
    uint64_t num_elements;
    guarantee_deserialization(deserialize_varint_uint64(&sz_read_stream, &num_elements),
                              "datum decode array");
    guarantee(num_elements <= std::numeric_limits<size_t>::max());
    return static_cast<size_t>(num_elements);
}

/* The format of `array` is:
     varint num_elements
     uint32_t offsets[num_elements - 1] // counted from `data`, first element omitted
     T data[num_elements] */
size_t datum_get_element_offset(const shared_buf_ref_t<char> &array, size_t index) {
    buffer_read_stream_t sz_read_stream(array.get(), array.get_safety_boundary());
    uint64_t num_elements;
    guarantee_deserialization(deserialize_varint_uint64(&sz_read_stream, &num_elements),
                              "datum decode array");
    guarantee(num_elements <= std::numeric_limits<size_t>::max());
    const size_t sz = static_cast<size_t>(num_elements);

    guarantee(index < sz);

    rassert(sz > 0);
    const size_t data_offset =
        static_cast<size_t>(sz_read_stream.tell())
        + (num_elements - 1) * serialize_universal_size_t<uint32_t>::value;

    if (index == 0) {
        return data_offset;
    } else {
        const size_t element_offset_offset =
            static_cast<size_t>(sz_read_stream.tell())
            + (index - 1) * serialize_universal_size_t<uint32_t>::value;

        array.guarantee_in_boundary(element_offset_offset);
        buffer_read_stream_t read_stream(
            array.get() + element_offset_offset,
            array.get_safety_boundary() - element_offset_offset);
        uint32_t element_offset;
        guarantee_deserialization(deserialize_universal(&read_stream, &element_offset),
                                  "datum decode array");
        return data_offset + element_offset;
    }
}

size_t datum_serialized_size(const datum_string_t &s) {
    const size_t s_size = s.size();
    return varint_uint64_serialized_size(s_size) + s_size;
}

serialization_result_t datum_serialize(write_message_t *wm, const datum_string_t &s) {
    const size_t s_size = s.size();
    serialize_varint_uint64(wm, static_cast<uint64_t>(s_size));
    wm->append(s.data(), s_size);
    return serialization_result_t::SUCCESS;
}

MUST_USE archive_result_t datum_deserialize(
        read_stream_t *s,
        datum_string_t *out) {
    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (res != archive_result_t::SUCCESS) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    const size_t str_offset = varint_uint64_serialized_size(sz);
    counted_t<shared_buf_t> buf =
        shared_buf_t::create(str_offset + static_cast<size_t>(sz));
    serialize_varint_uint64_into_buf(sz, reinterpret_cast<uint8_t *>(buf->data()));
    int64_t num_read = force_read(s, buf->data() + str_offset, sz);
    if (num_read == -1) {
        return archive_result_t::SOCK_ERROR;
    }
    if (static_cast<uint64_t>(num_read) < sz) {
        return archive_result_t::SOCK_EOF;
    }

    *out = datum_string_t(shared_buf_ref_t<char>(std::move(buf), 0));

    return archive_result_t::SUCCESS;
}


template <cluster_version_t W>
void serialize(write_message_t *wm,
               const empty_ok_t<const datum_t> &datum) {
    const datum_t *pointer = datum.get();
    const bool has = pointer->has();
    serialize<W>(wm, has);
    if (has) {
        serialize<W>(wm, *pointer);
    }
}

INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(empty_ok_t<const datum_t>);

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s,
                             empty_ok_ref_t<datum_t> datum) {
    bool has;
    archive_result_t res = deserialize<W>(s, &has);
    if (bad(res)) {
        return res;
    }

    datum_t *pointer = datum.get();

    if (!has) {
        pointer->reset();
        return archive_result_t::SUCCESS;
    } else {
        return deserialize<W>(s, pointer);
    }
}

template archive_result_t deserialize<cluster_version_t::v1_13>(
        read_stream_t *s,
        empty_ok_ref_t<datum_t> datum);
template archive_result_t deserialize<cluster_version_t::v1_13_2>(
        read_stream_t *s,
        empty_ok_ref_t<datum_t> datum);
template archive_result_t deserialize<cluster_version_t::v1_14>(
        read_stream_t *s,
        empty_ok_ref_t<datum_t> datum);
template archive_result_t deserialize<cluster_version_t::v1_15_is_latest>(
        read_stream_t *s,
        empty_ok_ref_t<datum_t> datum);


}  // namespace ql
