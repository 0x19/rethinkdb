// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "concurrency/watchable_map.hpp"

template<class key_t, class value_t>
watchable_map_t<key_t, value_t>::all_subs_t::all_subs_t(
        watchable_map_t<key_t, value_t> *map,
        const std::function<void(const key_t &key, const value_t *maybe_value)> &cb,
        const std::function<void(const std::map<key_t, value_t> &)> &initial_cb) :
        subscription(cb) {
    rwi_lock_assertion_t::read_acq_t acq(&map->rwi_lock);
    subscription.reset(map->all_subs_publisher.get_publisher());
    if (static_cast<bool>(initial_cb)) {
        map->read_all(initial_cb);
    }
}

template<class key_t, class value_t>
watchable_map_t<key_t, value_t>::key_subs_t::key_subs_t(
        watchable_map_t<key_t, value_t> *map,
        const key_t &key,
        const std::function<void(const value_t *maybe_value)> &cb,
        bool initial_call) :
        sentry(&map->key_subs_map, key, cb) {
    if (initial_call) {
        map->read_key(key, cb);
    }
}

template<class key_t, class value_t>
void watchable_map_t<key_t, value_t>::notify_change(
        const key_t &key,
        const value_t *new_value,
        rwi_lock_assertion_t::write_acq_t *write_acq) {
    write_acq->assert_is_holding(&rwi_lock);
    all_subs_publisher.publish(
        [&](const std::function<void(const key_t &, const value_t *)> &callback) {
            callback(key, new_value);
        });
    for (auto it = key_subs_map.lower_bound(key);
            it = key_subs_map.upper_bound(key);
            ++it) {
        it->second(new_value);
    }
}

template<class key_t, class value_t>
std::map<key_t, value_t> watchable_map_var_t<key_t, value_t>::get_all() {
    return map;
}

template<class key_t, class value_t>
boost::optional<value_t> watchable_map_var_t<key_t, value_t>::get_key(const key_t &key) {
    auto it = map.find(key);
    if (it == map.end()) {
        return boost::optional<value_t>();
    } else {
        return boost::optional<value_t>(it->second);
    }
}

template<class key_t, class value_t>
void watchable_map_var_t<key_t, value_t>::read_all(
        const std::function<void(const std::map<key_t, value_t> &)> &fun) {
    fun(map);
}

template<class key_t, class value_t>
void watchable_map_t<key_t, value_t>::read_key(
        const key_t &key,
        const std::function<void(const value_t *)> &fun) {
    auto it = map.find(key);
    if (it == map.end()) {
        fun(nullptr);
    } else {
        fun(&it->second);
    }
}



template<class key_t, class value_t>
void watchable_map_var_t<key_t, value_t>::set_all(
        const std::map<key_t, value_t> &new_value) {
    for (auto it = map.begin(); it != map.end();) {
        auto jt = it;
        ++it;
        if (new_value.count(jt->first) == 0) {
            delete_key(jt->first);
        }
    }
    for (auto it = new_value.begin(); it != new_value.end(); ++it) {
        set_key_no_equals(it->first, it->second);
    }
}

template<class key_t, class value_t>
void watchable_map_var_t<key_t, value_t>::set_key(
        const key_t &key, const value_t &new_value) {
    rwi_lock_assertion_t::write_acq_t write_acq(&rwi_lock);
    auto it = map.find(key);
    if (it == map.end()) {
        map.insert(std::make_pair(key, new_value));
        notify_change(key, &new_value, &write_acq);
    } else {
        if (it->second == new_value) {
            return;
        }
        it->second = new_value;
        notify_change(key, &new_value, &write_acq);
    }
}

template<class key_t, class value_t>
void watchable_map_var_t<key_t, value_t>::set_key_no_equals(
        const key_t &key, const value_t &new_value) {
    rwi_lock_assertion_t::write_acq_t write_acq(&rwi_lock);
    map.insert(std::make_pair(key, new_value));
    notify_change(key, &new_value, &write_acq);
}

template<class key_t, class value_t>
void watchable_map_var_t<key_t, value_t>::delete_key(const key_t &key) {
    rwi_lock_assertion_t::write_acq_t write_acq(&rwi_lock);
    map.erase(key);
    notify_change(key, nullptr, &write_acq);
}

