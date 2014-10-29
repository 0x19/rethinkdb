// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/real_reql_cluster_interface.hpp"

#include "clustering/administration/artificial_reql_cluster_interface.hpp"
#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/main/watchable_fields.hpp"
#include "clustering/administration/servers/name_client.hpp"
#include "clustering/administration/tables/generate_config.hpp"
#include "clustering/administration/tables/split_points.hpp"
#include "clustering/administration/tables/table_config.hpp"
#include "concurrency/cross_thread_signal.hpp"
#include "rdb_protocol/artificial_table/artificial_table.hpp"
#include "rdb_protocol/val.hpp"
#include "rdb_protocol/wait_for_readiness.hpp"
#include "rpc/semilattice/watchable.hpp"
#include "rpc/semilattice/view/field.hpp"

#define NAMESPACE_INTERFACE_EXPIRATION_MS (60 * 1000)

real_reql_cluster_interface_t::real_reql_cluster_interface_t(
        mailbox_manager_t *_mailbox_manager,
        boost::shared_ptr<
            semilattice_readwrite_view_t<cluster_semilattice_metadata_t> > _semilattices,
        watchable_map_t<std::pair<peer_id_t, namespace_id_t>,
                        namespace_directory_metadata_t> *_directory_root_view,
        rdb_context_t *_rdb_context,
        server_name_client_t *_server_name_client
        ) :
    mailbox_manager(_mailbox_manager),
    semilattice_root_view(_semilattices),
    directory_root_view(_directory_root_view),
    cross_thread_namespace_watchables(get_num_threads()),
    cross_thread_database_watchables(get_num_threads()),
    rdb_context(_rdb_context),
    namespace_repo(
        mailbox_manager,
        metadata_field(
            &cluster_semilattice_metadata_t::rdb_namespaces, semilattice_root_view),
        directory_root_view,
        rdb_context),
    changefeed_client(mailbox_manager,
        [this](const namespace_id_t &id, signal_t *interruptor) {
            return this->namespace_repo.get_namespace_interface(id, interruptor);
        }),
    server_name_client(_server_name_client)
{
    for (int thr = 0; thr < get_num_threads(); ++thr) {
        cross_thread_namespace_watchables[thr].init(
            new cross_thread_watchable_variable_t<cow_ptr_t<namespaces_semilattice_metadata_t> >(
                clone_ptr_t<semilattice_watchable_t<cow_ptr_t<namespaces_semilattice_metadata_t> > >
                    (new semilattice_watchable_t<cow_ptr_t<namespaces_semilattice_metadata_t> >(
                        metadata_field(&cluster_semilattice_metadata_t::rdb_namespaces, semilattice_root_view))), threadnum_t(thr)));

        cross_thread_database_watchables[thr].init(
            new cross_thread_watchable_variable_t<databases_semilattice_metadata_t>(
                clone_ptr_t<semilattice_watchable_t<databases_semilattice_metadata_t> >
                    (new semilattice_watchable_t<databases_semilattice_metadata_t>(
                        metadata_field(&cluster_semilattice_metadata_t::databases, semilattice_root_view))), threadnum_t(thr)));
    }
}

bool real_reql_cluster_interface_t::db_create(const name_string_t &name,
        signal_t *interruptor, std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();
        metadata_searcher_t<database_semilattice_metadata_t> db_searcher(
            &metadata.databases.databases);

        metadata_search_status_t status;
        db_searcher.find_uniq(name, &status);
        if (!check_metadata_status(status, "Database", name.str(), false, error_out)) {
            return false;
        }

        database_semilattice_metadata_t db;
        db.name = versioned_t<name_string_t>(name);
        metadata.databases.databases.insert(
            std::make_pair(generate_uuid(), make_deletable(db)));

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();
    }
    wait_for_metadata_to_propagate(metadata, interruptor);

    return true;
}

bool real_reql_cluster_interface_t::db_drop(const name_string_t &name,
        signal_t *interruptor, std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();
        metadata_searcher_t<database_semilattice_metadata_t> db_searcher(
            &metadata.databases.databases);

        metadata_search_status_t status;
        auto it = db_searcher.find_uniq(name, &status);
        if (!check_metadata_status(status, "Database", name.str(), true, error_out)) {
            return false;
        }

        /* Delete the database */
        deletable_t<database_semilattice_metadata_t> *db_metadata = &it->second;
        guarantee(!db_metadata->is_deleted());
        db_metadata->mark_deleted();

        /* Delete all of the tables in the database */
        cow_ptr_t<namespaces_semilattice_metadata_t>::change_t ns_change(
            &metadata.rdb_namespaces);
        metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
            &ns_change.get()->namespaces);
        namespace_predicate_t pred(&it->first);
        for (auto it2 = ns_searcher.find_next(ns_searcher.begin(), pred);
                  it2 != ns_searcher.end();
                  it2 = ns_searcher.find_next(++it2, pred)) {
            guarantee(!it2->second.is_deleted());
            it2->second.mark_deleted();
        }

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();
    }
    wait_for_metadata_to_propagate(metadata, interruptor);

    return true;
}

bool real_reql_cluster_interface_t::db_list(
        UNUSED signal_t *interruptor, std::set<name_string_t> *names_out,
        UNUSED std::string *error_out) {
    databases_semilattice_metadata_t db_metadata;
    get_databases_metadata(&db_metadata);
    const_metadata_searcher_t<database_semilattice_metadata_t> db_searcher(
        &db_metadata.databases);
    for (auto it = db_searcher.find_next(db_searcher.begin());
              it != db_searcher.end();
              it = db_searcher.find_next(++it)) {
        guarantee(!it->second.is_deleted());
        names_out->insert(it->second.get_ref().name.get_ref());
    }

    return true;
}

bool real_reql_cluster_interface_t::db_find(const name_string_t &name,
        UNUSED signal_t *interruptor, counted_t<const ql::db_t> *db_out,
        std::string *error_out) {
    guarantee(name != name_string_t::guarantee_valid("rethinkdb"),
        "real_reql_cluster_interface_t should never get queries for system tables");
    /* Find the specified database */
    databases_semilattice_metadata_t db_metadata;
    get_databases_metadata(&db_metadata);
    const_metadata_searcher_t<database_semilattice_metadata_t> db_searcher(
        &db_metadata.databases);
    metadata_search_status_t status;
    auto it = db_searcher.find_uniq(name, &status);
    if (!check_metadata_status(status, "Database", name.str(), true, error_out)) {
        return false;
    }
    *db_out = make_counted<const ql::db_t>(it->first, name.str());
    return true;
}

bool real_reql_cluster_interface_t::table_create(const name_string_t &name,
        counted_t<const ql::db_t> db,
        UNUSED const boost::optional<name_string_t> &primary_dc,
        bool hard_durability, const std::string &primary_key, signal_t *interruptor,
        std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    namespace_id_t namespace_id;
    {
        cross_thread_signal_t interruptor2(interruptor,
            semilattice_root_view->home_thread());
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();

        /* Find the specified datacenter */
#if 0 /* RSI: Figure out what to do about datacenters */
        uuid_u dc_id;
        if (primary_dc) {
            metadata_searcher_t<datacenter_semilattice_metadata_t> dc_searcher(
                &metadata.datacenters.datacenters);
            metadata_search_status_t status;
            auto it = dc_searcher.find_uniq(*primary_dc, &status);
            if (!check_metadata_status(status, "Datacenter", primary_dc->str(), true,
                    error_out)) return false;
            dc_id = it->first;
        } else {
            dc_id = nil_uuid();
        }
#endif /* 0 */

        cow_ptr_t<namespaces_semilattice_metadata_t>::change_t ns_change(
            &metadata.rdb_namespaces);
        metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
            &ns_change.get()->namespaces);

        /* Make sure there isn't an existing table with the same name */
        {
            metadata_search_status_t status;
            namespace_predicate_t pred(&name, &db->id);
            ns_searcher.find_uniq(pred, &status);
            if (!check_metadata_status(status, "Table", db->name + "." + name.str(),
                false, error_out)) return false;
        }

        table_replication_info_t repli_info;

        /* We can't meaningfully pick shard points, so create only one shard */
        repli_info.shard_scheme = table_shard_scheme_t::one_shard();

        /* Construct a configuration for the new namespace */
        std::map<server_id_t, int> server_usage;
        for (auto it = ns_change.get()->namespaces.begin();
                  it != ns_change.get()->namespaces.end();
                ++it) {
            if (it->second.is_deleted()) {
                continue;
            }
            calculate_server_usage(
                it->second.get_ref().replication_info.get_ref().config, &server_usage);
        }
        /* RSI(reql_admin): These should be passed by the user. */
        table_generate_config_params_t config_params;
        config_params.num_shards = 1;
        config_params.num_replicas[name_string_t::guarantee_valid("default")] = 1;
        config_params.director_tag = name_string_t::guarantee_valid("default");
        if (!table_generate_config(
                server_name_client, nil_uuid(), nullptr, server_usage,
                config_params, repli_info.shard_scheme, &interruptor2,
                &repli_info.config, error_out)) {
            return false;
        }

        namespace_semilattice_metadata_t table_metadata;
        table_metadata.name = versioned_t<name_string_t>(name);
        table_metadata.database = versioned_t<database_id_t>(db->id);
        table_metadata.primary_key = versioned_t<std::string>(primary_key);
        table_metadata.replication_info =
            versioned_t<table_replication_info_t>(repli_info);

        /* RSI(reql_admin): Figure out what to do with `hard_durability`. */
        (void)hard_durability;

        namespace_id = generate_uuid();
        ns_change.get()->namespaces.insert(
            std::make_pair(namespace_id, make_deletable(table_metadata)));

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();

        wait_for_metadata_to_propagate(metadata, &interruptor2);

        std::string error;
        UNUSED bool wait_res = table_wait(db, std::set<name_string_t>({name}),
                                          ql::make_counted_backtrace(), &interruptor2, NULL, &error);
        rassert(wait_res, "Failed to wait for table readiness: %s", error.c_str());
    }
    wait_for_metadata_to_propagate(metadata, interruptor);
    return true;
}

bool real_reql_cluster_interface_t::table_drop(const name_string_t &name,
        counted_t<const ql::db_t> db, signal_t *interruptor, std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");
    cluster_semilattice_metadata_t metadata;
    {
        on_thread_t thread_switcher(semilattice_root_view->home_thread());
        metadata = semilattice_root_view->get();

        /* Find the specified table */
        cow_ptr_t<namespaces_semilattice_metadata_t>::change_t ns_change(
                &metadata.rdb_namespaces);
        metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
                &ns_change.get()->namespaces);
        metadata_search_status_t status;
        namespace_predicate_t pred(&name, &db->id);
        metadata_searcher_t<namespace_semilattice_metadata_t>::iterator
            ns_metadata = ns_searcher.find_uniq(pred, &status);
        if (!check_metadata_status(status, "Table", db->name + "." + name.str(), true,
                error_out)) return false;
        guarantee(!ns_metadata->second.is_deleted());

        /* Delete the table. */
        ns_metadata->second.mark_deleted();

        semilattice_root_view->join(metadata);
        metadata = semilattice_root_view->get();
    }
    wait_for_metadata_to_propagate(metadata, interruptor);

    return true;
}

bool real_reql_cluster_interface_t::table_list(counted_t<const ql::db_t> db,
        UNUSED signal_t *interruptor, std::set<name_string_t> *names_out,
        UNUSED std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");
    cow_ptr_t<namespaces_semilattice_metadata_t> ns_metadata = get_namespaces_metadata();
    const_metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
        &ns_metadata->namespaces);
    namespace_predicate_t pred(&db->id);
    for (auto it = ns_searcher.find_next(ns_searcher.begin(), pred);
              it != ns_searcher.end();
              it = ns_searcher.find_next(++it, pred)) {
        guarantee(!it->second.is_deleted());
        names_out->insert(it->second.get_ref().name.get_ref());
    }

    return true;
}

bool real_reql_cluster_interface_t::table_find(const name_string_t &name,
        counted_t<const ql::db_t> db, signal_t *interruptor,
        scoped_ptr_t<base_table_t> *table_out, std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");
    /* Find the specified table in the semilattice metadata */
    cow_ptr_t<namespaces_semilattice_metadata_t> namespaces_metadata
        = get_namespaces_metadata();
    const_metadata_searcher_t<namespace_semilattice_metadata_t>
        ns_searcher(&namespaces_metadata.get()->namespaces);
    namespace_predicate_t pred(&name, &db->id);
    metadata_search_status_t status;
    auto ns_metadata_it = ns_searcher.find_uniq(pred, &status);
    if (!check_metadata_status(status, "Table", db->name + "." + name.str(), true,
            error_out)) return false;
    guarantee(!ns_metadata_it->second.is_deleted());

    table_out->init(new real_table_t(
        ns_metadata_it->first,
        namespace_repo.get_namespace_interface(ns_metadata_it->first, interruptor),
        ns_metadata_it->second.get_ref().primary_key.get_ref(),
        &changefeed_client));

    return true;
}

bool real_reql_cluster_interface_t::get_table_ids_for_query(
        counted_t<const ql::db_t> db,
        const std::set<name_string_t> &table_names,
        std::set<namespace_id_t> *table_ids_out,
        std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");

    table_ids_out->clear();
    cow_ptr_t<namespaces_semilattice_metadata_t> ns_metadata = get_namespaces_metadata();
    const_metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
        &ns_metadata->namespaces);

    if (table_names.empty()) {
        namespace_predicate_t pred(&db->id);
        for (auto it = ns_searcher.find_next(ns_searcher.begin(), pred);
                  it != ns_searcher.end();
                  it = ns_searcher.find_next(++it, pred)) {
            guarantee(!it->second.is_deleted());
            table_ids_out->insert(it->first);
        }
    } else {
        for (auto const &name : table_names) {
            namespace_predicate_t pred(&name, &db->id);
            metadata_search_status_t status;
            auto it = ns_searcher.find_uniq(pred, &status);
            if (!check_metadata_status(status, "Table", db->name + "." + name.str(), true,
                    error_out)) return false;
            guarantee(!it->second.is_deleted());
            table_ids_out->insert(it->first);
        }
    }
    return true;
}

bool real_reql_cluster_interface_t::table_config(
        counted_t<const ql::db_t> db,
        const std::set<name_string_t> &tables,
        const ql::protob_t<const Backtrace> &bt,
        signal_t *interruptor, scoped_ptr_t<ql::val_t> *resp_out,
        std::string *error_out) {
    std::set<namespace_id_t> table_ids;
    if (!get_table_ids_for_query(db, tables, &table_ids, error_out)) {
        return false;
    }

    return table_meta_read(
        admin_tables->table_config_backend.get(), "table_config",
        table_ids, bt, interruptor, resp_out, error_out);
}

bool real_reql_cluster_interface_t::table_status(
        counted_t<const ql::db_t> db,
        const std::set<name_string_t> &tables,
        const ql::protob_t<const Backtrace> &bt,
        signal_t *interruptor, scoped_ptr_t<ql::val_t> *resp_out,
        std::string *error_out) {
    std::set<namespace_id_t> table_ids;
    if (!get_table_ids_for_query(db, tables, &table_ids, error_out)) {
        return false;
    }

    return table_meta_read(
        admin_tables->table_status_backend.get(), "table_status",
        table_ids, bt, interruptor, resp_out, error_out);
}

class table_waiter_t {
public:
    table_waiter_t(const namespace_id_t &_table_id,
                   watchable_map_t<std::pair<peer_id_t, namespace_id_t>,
                                   namespace_directory_metadata_t> *directory,
                   cross_thread_watchable_variable_t<cow_ptr_t<namespaces_semilattice_metadata_t> >
                       *_namespaces_watchable) :
        table_id(_table_id),
        table_directory(directory, table_id),
        namespaces_watchable(_namespaces_watchable) { }

    enum class waited_t {
        WAITED,
        IMMEDIATE
    };

    waited_t wait_ready(signal_t *interruptor) {
        int num_checks = 0;
        table_directory.run_all_until_satisfied(
            [&](watchable_map_t<peer_id_t, namespace_directory_metadata_t> *d) -> bool {
                ++num_checks;
                return do_check(d);
            },
            interruptor);
        return num_checks > 1 ? waited_t::WAITED : waited_t::IMMEDIATE;
    }

    bool check_ready() {
        return do_check(&table_directory);
    }

private:
    bool do_check(watchable_map_t<peer_id_t, namespace_directory_metadata_t> *dir) {
        // First make sure the table was not deleted
        bool is_deleted;
        namespaces_watchable->apply_read(
            [&](const cow_ptr_t<namespaces_semilattice_metadata_t> *ns_md) {
                guarantee((*ns_md)->namespaces.count(table_id) == 1);
                is_deleted = (*ns_md)->namespaces.at(table_id).is_deleted();
            });
        if (is_deleted) {
            return true;
        }

        std::vector<region_t> regions;
        dir->read_all(
            [&](const peer_id_t &, const namespace_directory_metadata_t *metadata) {
                for (auto const &it : metadata->internal->activities) {
                    const reactor_business_card_t::primary_t *primary =
                        boost::get<reactor_business_card_t::primary_t>(&it.second.activity);
                    if (primary != NULL && primary->master) {
                        regions.push_back(primary->master.get().region);
                    }
                }
            });

        region_t whole;
        region_join_result_t res = region_join(regions, &whole);
        return (res == REGION_JOIN_OK && whole == region_t::universe());
    }

    namespace_id_t table_id;

    // TODO: this is copy/pasta from namespace_interface_repository_t - consolidate?
    class table_directory_t :
        public watchable_map_transform_t<
            std::pair<peer_id_t, namespace_id_t>,
            namespace_directory_metadata_t,
            peer_id_t,
            namespace_directory_metadata_t>
    {
    public:
        table_directory_t(
                watchable_map_t<std::pair<peer_id_t, namespace_id_t>,
                                namespace_directory_metadata_t> *_directory,
                namespace_id_t _namespace_id) :
            watchable_map_transform_t(_directory),
            nid(_namespace_id) { }
        bool key_1_to_2(const std::pair<peer_id_t, namespace_id_t> &key1,
                        peer_id_t *key2_out) {
            if (key1.second == nid) {
                *key2_out = key1.first;
                return true;
            } else {
                return false;
            }
        }
        void value_1_to_2(const namespace_directory_metadata_t *value1,
                          const namespace_directory_metadata_t **value2_out) {
            *value2_out = value1;
        }
        bool key_2_to_1(const peer_id_t &key2,
                        std::pair<peer_id_t, namespace_id_t> *key1_out) {
            key1_out->first = key2;
            key1_out->second = nid;
            return true;
        }
        namespace_id_t nid;
    } table_directory;

    cross_thread_watchable_variable_t<cow_ptr_t<namespaces_semilattice_metadata_t> >
        *namespaces_watchable;
};

bool real_reql_cluster_interface_t::table_wait(
        counted_t<const ql::db_t> db,
        const std::set<name_string_t> &tables,
        const ql::protob_t<const Backtrace> &bt,
        signal_t *interruptor,
        scoped_ptr_t<ql::val_t> *resp_out,
        std::string *error_out) {
    std::set<namespace_id_t> table_ids;
    if (!get_table_ids_for_query(db, tables, &table_ids, error_out)) {
        return false;
    }

    std::vector<scoped_ptr_t<table_waiter_t> > waiters;
    for (auto const &id : table_ids) {
        waiters.push_back(scoped_ptr_t<table_waiter_t>(
            new table_waiter_t(id, directory_root_view,
                cross_thread_namespace_watchables[get_thread_id().threadnum].get())));
    }

    // Loop until all tables are ready
    while (true) {
        bool immediate = true;
        for (auto const &w : waiters) {
            table_waiter_t::waited_t res = w->wait_ready(interruptor);
            immediate = immediate && (res == table_waiter_t::waited_t::IMMEDIATE);
        }

        if (!immediate && waiters.size() > 1) {
            // Do a second pass to make sure no tables changed while we were waiting
            bool redo = false;
            for (auto const &w : waiters) {
                if (!w->check_ready()) {
                    redo = true;
                    break;
                }
            }

            if (redo) {
                continue;
            }
        }
        break;
    } 

    if (resp_out == NULL) {
        return true;
    }

    return table_meta_read(
        admin_tables->table_status_backend.get(), "table_wait",
        table_ids, bt, interruptor, resp_out, error_out);
}

bool real_reql_cluster_interface_t::table_reconfigure(
        counted_t<const ql::db_t> db,
        const name_string_t &name,
        const table_generate_config_params_t &params,
        bool dry_run,
        signal_t *interruptor,
        ql::datum_t *new_config_out,
        std::string *error_out) {
    guarantee(db->name != "rethinkdb",
        "real_reql_cluster_interface_t should never get queries for system tables");
    cross_thread_signal_t interruptor2(interruptor, server_name_client->home_thread());
    on_thread_t thread_switcher(server_name_client->home_thread());

    /* Find the specified table in the semilattice metadata */
    cluster_semilattice_metadata_t metadata = semilattice_root_view->get();
    cow_ptr_t<namespaces_semilattice_metadata_t>::change_t ns_change(
            &metadata.rdb_namespaces);
    metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
            &ns_change.get()->namespaces);
    namespace_predicate_t pred(&name, &db->id);
    metadata_search_status_t status;
    auto ns_metadata_it = ns_searcher.find_uniq(pred, &status);
    if (!check_metadata_status(status, "Table", db->name + "." + name.str(), true,
            error_out)) return false;

    std::map<server_id_t, int> server_usage;
    for (auto it = ns_searcher.find_next(ns_searcher.begin());
              it != ns_searcher.end();
              it = ns_searcher.find_next(++it)) {
        if (it == ns_metadata_it) {
            /* We don't want to take into account the table's current configuration,
            since we're about to change that anyway */
            continue;
        }
        calculate_server_usage(it->second.get_ref().replication_info.get_ref().config,
                               &server_usage);
    }

    table_replication_info_t new_repli_info;

    if (!calculate_split_points_intelligently(
            ns_metadata_it->first,
            this,
            params.num_shards,
            ns_metadata_it->second.get_ref().replication_info.get_ref().shard_scheme,
            &interruptor2,
            &new_repli_info.shard_scheme,
            error_out)) {
        return false;
    }

    /* This just generates a new configuration; it doesn't put it in the
    semilattices. */
    if (!table_generate_config(
            server_name_client,
            ns_metadata_it->first,
            directory_root_view,
            server_usage,
            params,
            new_repli_info.shard_scheme,
            &interruptor2,
            &new_repli_info.config,
            error_out)) {
        return false;
    }

    if (!dry_run) {
        /* Commit the change */
        ns_metadata_it->second.get_mutable()->replication_info.set(new_repli_info);
        semilattice_root_view->join(metadata);
    }

    *new_config_out = convert_table_config_to_datum(
        new_repli_info.config, server_name_client);

    return true;
}

/* Checks that divisor is indeed a divisor of multiple. */
template <class T>
bool is_joined(const T &multiple, const T &divisor) {
    T cpy = multiple;

    semilattice_join(&cpy, divisor);
    return cpy == multiple;
}

void real_reql_cluster_interface_t::wait_for_metadata_to_propagate(
        const cluster_semilattice_metadata_t &metadata, signal_t *interruptor) {
    int threadnum = get_thread_id().threadnum;

    guarantee(cross_thread_namespace_watchables[threadnum].has());
    cross_thread_namespace_watchables[threadnum]->get_watchable()->run_until_satisfied(
            [&] (const cow_ptr_t<namespaces_semilattice_metadata_t> &md) -> bool
                { return is_joined(md, metadata.rdb_namespaces); },
            interruptor);

    guarantee(cross_thread_database_watchables[threadnum].has());
    cross_thread_database_watchables[threadnum]->get_watchable()->run_until_satisfied(
            [&] (const databases_semilattice_metadata_t &md) -> bool
                { return is_joined(md, metadata.databases); },
            interruptor);
}

template <class T>
void copy_value(const T *in, T *out) {
    *out = *in;
}

cow_ptr_t<namespaces_semilattice_metadata_t>
real_reql_cluster_interface_t::get_namespaces_metadata() {
    int threadnum = get_thread_id().threadnum;
    r_sanity_check(cross_thread_namespace_watchables[threadnum].has());
    cow_ptr_t<namespaces_semilattice_metadata_t> ret;
    cross_thread_namespace_watchables[threadnum]->apply_read(
            std::bind(&copy_value< cow_ptr_t<namespaces_semilattice_metadata_t> >,
                      ph::_1, &ret));
    return ret;
}

void real_reql_cluster_interface_t::get_databases_metadata(
        databases_semilattice_metadata_t *out) {
    int threadnum = get_thread_id().threadnum;
    r_sanity_check(cross_thread_database_watchables[threadnum].has());
    cross_thread_database_watchables[threadnum]->apply_read(
            std::bind(&copy_value<databases_semilattice_metadata_t>,
                      ph::_1, out));
}

bool real_reql_cluster_interface_t::table_meta_read(
        artificial_table_backend_t *backend, const char *backend_name,
        const std::set<namespace_id_t> &table_ids,
        const ql::protob_t<const Backtrace> &bt,
        signal_t *interruptor, scoped_ptr_t<ql::val_t> *resp_out,
        std::string *error_out) {
    counted_t<ql::table_t> table = make_counted<ql::table_t>(
        scoped_ptr_t<base_table_t>(new artificial_table_t(backend)),
        make_counted<const ql::db_t>(nil_uuid(), "rethinkdb"), backend_name, false, bt);
    if (table_ids.size() == 1) {
        ql::datum_t pkey = convert_uuid_to_datum(*table_ids.begin());
        ql::datum_t row;
        if (!backend->read_row(pkey, interruptor, &row, error_out)) {
            return false;
        }
        resp_out->init(new ql::val_t(row, pkey, table, bt));
        return true;
    } else {
        ql::datum_array_builder_t array_builder(ql::configured_limits_t::unlimited);
        for (auto const &id : table_ids) {
            ql::datum_t row;
            if (!backend->read_row(convert_uuid_to_datum(id), interruptor, &row, error_out)) {
                return false;
            }
            array_builder.add(row);
        }
        counted_t<ql::datum_stream_t> stream = make_counted<ql::array_datum_stream_t>(
            std::move(array_builder).to_datum(), bt);
        resp_out->init(new ql::val_t(table, stream, bt));
        return true;
    }
}

