// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef RPC_DIRECTORY_WRITE_MANAGER_TCC_
#define RPC_DIRECTORY_WRITE_MANAGER_TCC_

#include "rpc/directory/write_manager.hpp"

#include <map>
#include <set>

#include "arch/runtime/coroutines.hpp"
#include "containers/archive/versioned.hpp"

#define MAX_OUTSTANDING_DIRECTORY_WRITES 4

template<class metadata_t>
directory_write_manager_t<metadata_t>::directory_write_manager_t(
        connectivity_cluster_t *connectivity_cluster_,
        connectivity_cluster_t::message_tag_t message_tag_,
        const clone_ptr_t<watchable_t<metadata_t> > &value_) THROWS_NOTHING :
    connectivity_cluster(connectivity_cluster_),
    message_tag(message_tag_),
    value(value_),
    semaphore(MAX_OUTSTANDING_DIRECTORY_WRITES),
    value_change_subscription([this]() { this->on_value_change(); }),
    connections_change_subscription([this]() { this->on_connections_change(); })
{
    typename watchable_t<metadata_t>::freeze_t value_freeze(value);
    typename watchable_t<connectivity_cluster_t::connection_map_t>::freeze_t
        connections_freeze(connectivity_cluster->get_connections());
    guarantee(connectivity_cluster->get_connections()->get().empty());
    value_change_subscription.reset(value, &value_freeze);
    connections_change_subscription.reset(connectivity_cluster->get_connections(),
                                          &connections_freeze);
}

template<class metadata_t>
void directory_write_manager_t<metadata_t>::on_connections_change() THROWS_NOTHING {
    mutex_assertion_t::acq_t mutex_assertion_lock(&mutex_assertion);
    connectivity_cluster_t::connection_map_t current_connections =
        connectivity_cluster->get_connections()->get();
    for (auto pair : current_connections) {
        connectivity_cluster_t::connection_t *connection = pair.second.first;
        auto_drainer_t::lock_t connection_keepalive = pair.second.second;
        if (last_connections.count(connection) == 0) {
            last_connections.insert(std::make_pair(connection, connection_keepalive));
            auto_drainer_t::lock_t this_keepalive(&drainer);
            metadata_t initial_value = value.get()->get();
            fifo_enforcer_state_t initial_state = metadata_fifo_source.get_state();
            coro_t::spawn_sometime(
                [this, this_keepalive /* important to capture */,
                        connection, connection_keepalive /* important to capture */,
                        initial_value, initial_state]() {
                    new_semaphore_acq_t acq(&semaphore, 1);
                    acq.acquisition_signal()->wait();
                    initialization_writer_t writer(initial_value, initial_state);
                    connectivity_cluster->send_message(connection, connection_keepalive,
                            message_tag, &writer);
                });
        }
    }
    for (auto pair : last_connections) {
        if (current_connections.count(pair.first->get_peer_id()) == 0) {
            last_connections.erase(pair.first);
        }
    }
}

template<class metadata_t>
void directory_write_manager_t<metadata_t>::on_value_change() THROWS_NOTHING {
    mutex_assertion_t::acq_t mutex_assertion_lock(&mutex_assertion);
    metadata_t current_value = value.get()->get();
    fifo_enforcer_write_token_t token = metadata_fifo_source.enter_write();
    auto_drainer_t::lock_t this_keepalive(&drainer);
    for (auto pair : last_connections) {
        connectivity_cluster_t::connection_t *connection = pair.first;
        auto_drainer_t::lock_t connection_keepalive = pair.second;
        coro_t::spawn_sometime(
            [this, this_keepalive /* important to capture */,
                    connection, connection_keepalive /* important to capture */,
                    current_value, token]() {
                update_writer_t writer(current_value, token);
                connectivity_cluster->send_message(connection, connection_keepalive,
                        message_tag, &writer);
            });
    }
}

template <class metadata_t>
class directory_write_manager_t<metadata_t>::initialization_writer_t :
    public connectivity_cluster_t::send_message_write_callback_t
{
public:
    initialization_writer_t(const metadata_t &_initial_value,
                            fifo_enforcer_state_t _metadata_fifo_state) :
        initial_value(_initial_value), metadata_fifo_state(_metadata_fifo_state) { }
    ~initialization_writer_t() { }

    void write(cluster_version_t cluster_version, write_stream_t *stream) {
        write_message_t wm;
        // All cluster versions use a uint8_t code.
        const uint8_t code = 'I';
        serialize_universal(&wm, code);
        serialize_for_version(cluster_version, &wm, initial_value);
        serialize_for_version(cluster_version, &wm, metadata_fifo_state);
        int res = send_write_message(stream, &wm);
        if (res) {
            throw fake_archive_exc_t();
        }
    }
private:
    const metadata_t &initial_value;
    fifo_enforcer_state_t metadata_fifo_state;
};

template <class metadata_t>
class directory_write_manager_t<metadata_t>::update_writer_t :
    public connectivity_cluster_t::send_message_write_callback_t
{
public:
    update_writer_t(const metadata_t &_new_value,
                    fifo_enforcer_write_token_t _metadata_fifo_token) :
        new_value(_new_value), metadata_fifo_token(_metadata_fifo_token) { }
    ~update_writer_t() { }

    void write(cluster_version_t cluster_version, write_stream_t *stream) {
        write_message_t wm;
        // All cluster versions use a uint8_t code.
        const uint8_t code = 'U';
        serialize_universal(&wm, code);
        serialize_for_version(cluster_version, &wm, new_value);
        serialize_for_version(cluster_version, &wm, metadata_fifo_token);
        int res = send_write_message(stream, &wm);
        if (res) {
            throw fake_archive_exc_t();
        }
    }
private:
    const metadata_t &new_value;
    fifo_enforcer_write_token_t metadata_fifo_token;
};

#endif  // RPC_DIRECTORY_WRITE_MANAGER_TCC_
