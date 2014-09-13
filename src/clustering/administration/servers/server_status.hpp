// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_SERVERS_SERVER_STATUS_HPP_
#define CLUSTERING_ADMINISTRATION_SERVERS_SERVER_STATUS_HPP_

#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/shared_ptr.hpp>

#include "clustering/administration/metadata.hpp"
#include "clustering/administration/servers/server_common.hpp"
#include "clustering/administration/servers/machine_metadata.hpp"
#include "rdb_protocol/artificial_table/backend.hpp"
#include "rpc/semilattice/view.hpp"

class last_seen_tracker_t;
class server_name_client_t;

class server_status_artificial_table_backend_t :
    public common_server_artificial_table_backend_t
{
public:
    server_status_artificial_table_backend_t(
            boost::shared_ptr< semilattice_read_view_t<
                machines_semilattice_metadata_t> > _servers_sl_view,
            server_name_client_t *_name_client,
            clone_ptr_t< watchable_t< change_tracking_map_t<
                peer_id_t, cluster_directory_metadata_t> > > _directory_view,
            boost::shared_ptr< semilattice_readwrite_view_t<
                cow_ptr_t<namespaces_semilattice_metadata_t> > > _table_sl_view,
            boost::shared_ptr< semilattice_readwrite_view_t<
                databases_semilattice_metadata_t> > _database_sl_view,
            last_seen_tracker_t *_last_seen_tracker) :
        common_server_artificial_table_backend_t(_servers_sl_view, _name_client),
        directory_view(_directory_view), table_sl_view(_table_sl_view),
        database_sl_view(_database_sl_view),
        last_seen_tracker(_last_seen_tracker) {
        table_sl_view->assert_thread();
        database_sl_view->assert_thread();
    }

    bool read_row(
            ql::datum_t primary_key,
            signal_t *interruptor,
            ql::datum_t *row_out,
            std::string *error_out);
    bool write_row(
            ql::datum_t primary_key,
            ql::datum_t new_value,
            signal_t *interruptor,
            std::string *error_out);

private:
    name_string_t get_db_name(database_id_t db_id);

    clone_ptr_t< watchable_t< change_tracking_map_t<
        peer_id_t, cluster_directory_metadata_t> > > directory_view;
    boost::shared_ptr< semilattice_readwrite_view_t<
        cow_ptr_t<namespaces_semilattice_metadata_t> > > table_sl_view;
    boost::shared_ptr< semilattice_readwrite_view_t<
        databases_semilattice_metadata_t> > database_sl_view;
    last_seen_tracker_t *last_seen_tracker;
};

#endif /* CLUSTERING_ADMINISTRATION_SERVERS_SERVER_STATUS_HPP_ */

