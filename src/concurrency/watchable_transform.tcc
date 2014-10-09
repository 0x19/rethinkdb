// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "concurrency/watchable_transform.hpp"

#include "debug.hpp"
#include "stl_utils.hpp"

template<class key1_t, class value1_t, class key2_t, class value2_t>
watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::watchable_map_transform_t(
        watchable_map_t<key1_t, value1_t> *_inner) :
    inner(_inner),
    all_subs(inner,
        [this](const key1_t &key1, const value1_t *value1) {
            rwi_lock_assertion_t::write_acq_t write_acq(&rwi_lock);
            key2_t key2;
            if (key_1_to_2(key1, &key2)) {
                if (value1 != nullptr) {
                    const value2_t *value2;
                    value_1_to_2(value1, &value2);
                    guarantee(value2 != nullptr);
                    do_notify_change(key2, value2, &write_acq);
                } else {
                    do_notify_change(key2, nullptr, &write_acq);
                }
            }
        }, false)
    { }

template<class key1_t, class value1_t, class key2_t, class value2_t>
std::map<key2_t, value2_t>
watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::get_all() {
    std::map<key2_t, value2_t> map2;
    read_all([&](const key2_t &key2, const value2_t *value2) {
        auto pair = map2.insert(std::make_pair(key2, *value2));
        guarantee(pair.second, "key_1_to_2 created collision");
    });
    return map2;
}

template<class key1_t, class value1_t, class key2_t, class value2_t>
boost::optional<value2_t>
watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::get_key(
        const key2_t &key2) {
    key1_t key1;
    key_2_to_1(key2, &key1);
    boost::optional<value2_t> res;
    inner->read_key(key1, [&](const value1_t *value1) {
        if (value1 != nullptr) {
            const value2_t *value2;
            value_1_to_2(value1, &value2);
            guarantee(value2 != nullptr);
            res = boost::optional<value2_t>(*value2);
        }
    });
    return res;
}

template<class key1_t, class value1_t, class key2_t, class value2_t>
void watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::read_all(
        const std::function<void(const key2_t &, const value2_t *)> &cb) {
    inner->read_all([&](const key1_t &key1, const value1_t *value1) {
        key2_t key2;
        if (key_1_to_2(key1, &key2)) {
            const value2_t *value2;
            value_1_to_2(value1, &value2);
            cb(key2, value2);
        }
    });
}

template<class key1_t, class value1_t, class key2_t, class value2_t>
void watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::read_key(
        const key2_t &key2, const std::function<void(const value2_t *)> &cb) {
    key1_t key1;
    key_2_to_1(key2, &key1);
    inner->read_key(key1, [&](const value1_t *value1) {
        if (value1 != nullptr) {
            const value2_t *value2;
            value_1_to_2(value1, &value2);
            guarantee(value2 != nullptr);
            cb(value2);
        } else {
            cb(nullptr);
        }
    });
}

template<class key1_t, class value1_t, class key2_t, class value2_t>
rwi_lock_assertion_t *
watchable_map_transform_t<key1_t, value1_t, key2_t, value2_t>::get_rwi_lock() {
    return &rwi_lock;
}

template<class key_t, class value_t>
clone_ptr_t<watchable_t<boost::optional<value_t> > > get_watchable_for_key(
        watchable_map_t<key_t, value_t> *map,
        const key_t &key) {
    class w_t : public watchable_t<boost::optional<value_t> > {
    public:
        w_t(watchable_map_t<key_t, value_t> *_map, const key_t &_key) :
            map(_map), key(_key),
            subs(map, key,
                [this](const value_t *) {
                    rwi_lock_assertion_t::write_acq_t acq(&rwi_lock);
                    publisher.publish([](const std::function<void()> &f) { f(); });
                },
                false)
            { }
        w_t *clone() const {
            return new w_t(map, key);
        }
        boost::optional<value_t> get() {
            return map->get_key(key);
        }
        void apply_read(
                const std::function<void(const boost::optional<value_t> *)> &read) {
            boost::optional<value_t> value = get();
            read(&value);
        }
        publisher_t<std::function<void()> > *get_publisher() {
            return publisher.get_publisher();
        }
        rwi_lock_assertion_t *get_rwi_lock_assertion() {
            return &rwi_lock;
        }
    private:
        publisher_controller_t<std::function<void()> > publisher;
        rwi_lock_assertion_t rwi_lock;
        watchable_map_t<key_t, value_t> *map;
        key_t key;
        typename watchable_map_t<key_t, value_t>::key_subs_t subs;
    };
    return clone_ptr_t<watchable_t<boost::optional<value_t> > >(new w_t(map, key));
}

template<class key_t, class value_t>
watchable_map_entry_copier_t<key_t, value_t>::watchable_map_entry_copier_t(
        watchable_map_var_t<key_t, value_t> *_map,
        const key_t &_key,
        clone_ptr_t<watchable_t<value_t> > _value,
        bool _remove_when_done) :
    map(_map), key(_key), value(_value), remove_when_done(_remove_when_done),
    subs([this]() {
        map->set_key_no_equals(key, value->get());
    })
{
    typename watchable_t<value_t>::freeze_t freeze(value);
    map->set_key_no_equals(key, value->get());
    subs.reset(value, &freeze);
}

template<class key_t, class value_t>
watchable_map_entry_copier_t<key_t, value_t>::~watchable_map_entry_copier_t() {
    if (remove_when_done) {
        map->delete_key(key);
    }
}

