// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CONTAINERS_CLONE_PTR_HPP_
#define CONTAINERS_CLONE_PTR_HPP_

#include "containers/archive/archive.hpp"
#include "containers/scoped.hpp"
#include "rpc/serialize_macros.hpp"

/* `clone_ptr_t` is a smart pointer that calls the `clone()` method on its
underlying object whenever the `clone_ptr_t`'s copy constructor is called. It's
primarily useful when you have a type that effectively acts like a piece of
data (i.e. it can be meaningfully copied) but it also has virtual methods.
Remember to declare `clone()` as a virtual method! */

template<class T>
class clone_ptr_t {
public:
    clone_ptr_t() THROWS_NOTHING;

    /* Takes ownership of the argument. */
    explicit clone_ptr_t(T *) THROWS_NOTHING;  // NOLINT

    clone_ptr_t(const clone_ptr_t &x) THROWS_NOTHING;
    template<class U>
    clone_ptr_t(const clone_ptr_t<U> &x) THROWS_NOTHING;  // NOLINT(runtime/explicit)

    clone_ptr_t &operator=(const clone_ptr_t &x) THROWS_NOTHING;
    template<class U>
    clone_ptr_t &operator=(const clone_ptr_t<U> &x) THROWS_NOTHING;

    T &operator*() const THROWS_NOTHING;
    T *operator->() const THROWS_NOTHING;
    T *get() const THROWS_NOTHING;

    /* This mess is so that we can use `clone_ptr_t` in boolean contexts. */
    typedef void (clone_ptr_t::*booleanish_t)();
    operator booleanish_t() const THROWS_NOTHING;

private:
    template<class U> friend class clone_ptr_t;

    void truth_value_method_for_use_in_boolean_conversions();

    template <cluster_version_t W>
    void rdb_serialize(write_message_t *wm) const {
        // clone pointers own their pointees exclusively, so we don't
        // have to worry about replicating any boost pointer
        // serialization bullshit.
        bool exists = object;
        serialize<W>(wm, exists);
        if (exists) {
            serialize<W>(wm, *object);
        }
    }

    template <cluster_version_t W>
    archive_result_t rdb_deserialize(read_stream_t *s) {
        rassert(!object.has());
        object.reset();
        T *tmp;
        archive_result_t res = deserialize<W>(s, &tmp);
        object.init(tmp);
        return res;
    }

    scoped_ptr_t<T> object;
};

RDB_SERIALIZE_TEMPLATED_OUTSIDE(clone_ptr_t);

#include "containers/clone_ptr.tcc"

#endif /* CONTAINERS_CLONE_PTR_HPP_ */
