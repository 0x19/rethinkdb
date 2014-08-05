// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_NAMESPACE_INTERFACE_REPOSITORY_HPP_
#define CLUSTERING_ADMINISTRATION_NAMESPACE_INTERFACE_REPOSITORY_HPP_

#include <map>

#include "clustering/administration/metadata.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/one_per_thread.hpp"
#include "concurrency/promise.hpp"
#include "containers/clone_ptr.hpp"
#include "containers/incremental_lenses.hpp"
#include "rdb_protocol/real_table.hpp"

/* `namespace_repo_t` is a helper class for `real_reql_cluster_interface_t`. It's
responsible for constructing and caching `cluster_namespace_interface_t` objects. */

class mailbox_manager_t;
class namespace_interface_t;
class namespaces_directory_metadata_t;
class peer_id_t;
class rdb_context_t;
class signal_t;
class uuid_u;
template <class> class watchable_t;

class namespace_repo_t : public home_thread_mixin_t {
public:
    namespace_repo_t(mailbox_manager_t *,
                     const boost::shared_ptr<semilattice_read_view_t<cow_ptr_t<namespaces_semilattice_metadata_t> > > &semilattice_view,
                     clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> > >,
                     rdb_context_t *);
    ~namespace_repo_t();

    namespace_interface_access_t get_namespace_interface(const namespace_id_t &ns_id,
        signal_t *interruptor);

private:
    struct namespace_cache_t;
    struct namespace_cache_entry_t;

    void create_and_destroy_namespace_interface(
            namespace_cache_t *cache,
            const uuid_u &namespace_id,
            auto_drainer_t::lock_t keepalive)
            THROWS_NOTHING;
    void on_namespaces_change(auto_drainer_t::lock_t keepalive);

    mailbox_manager_t *mailbox_manager;
    boost::shared_ptr<semilattice_read_view_t<cow_ptr_t<namespaces_semilattice_metadata_t> > > namespaces_view;
    clone_ptr_t<watchable_t<change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> > > namespaces_directory_metadata;
    rdb_context_t *ctx;

    one_per_thread_t<std::map<namespace_id_t, std::map<key_range_t, machine_id_t> > >
        region_to_primary_maps;

    one_per_thread_t<namespace_cache_t> namespace_caches;

    DISABLE_COPYING(namespace_repo_t);

    auto_drainer_t drainer;

    // We must destroy the subscription before the drainer
    semilattice_read_view_t<cow_ptr_t<namespaces_semilattice_metadata_t> >::subscription_t namespaces_subscription;
};

#endif /* CLUSTERING_ADMINISTRATION_NAMESPACE_INTERFACE_REPOSITORY_HPP_ */
