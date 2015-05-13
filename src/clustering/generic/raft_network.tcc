// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_GENERIC_RAFT_NETWORK_TCC_
#define CLUSTERING_GENERIC_RAFT_NETWORK_TCC_

#include "clustering/generic/raft_network.hpp"

template<class state_t>
raft_networked_member_t<state_t>::raft_networked_member_t(
        const raft_member_id_t &this_member_id,
        mailbox_manager_t *_mailbox_manager,
        watchable_map_t<raft_member_id_t, raft_business_card_t<state_t> > *_bcards,
        raft_storage_interface_t<state_t> *storage,
        const raft_persistent_state_t<state_t> &persistent_state,
        const std::string &log_prefix) :
    mailbox_manager(_mailbox_manager),
    peers(_peers),
    member(this_member_id, storage, this, persistent_state, log_prefix),
    rpc_mailbox(mailbox_manager, std::bind(&raft_networked_member_t::on_rpc,
        this, ph::_1, ph::_2, ph::_3, ph::_4)),
    bcards_subs(peers, std::bind(&raft_networked_member_t::on_bcards_change,
        this, ph::_1, ph::_2), true),
    connections_subs(mailbox_manager->get_connectivity_cluster()->get_connections(),
        std::bind(&raft_networked_member_t::on_connections_change,
            this, ph::_1, ph::_2), true)
    { }

template<class state_t>
raft_business_card_t<state_t> raft_networked_member_t<state_t>::get_business_card() {
    raft_business_card_t<state_t> bc;
    bc.rpc = rpc_mailbox.get_address();
    return bc;
}

template<class state_t>
bool raft_networked_member_t<state_t>::send_rpc(
        const raft_member_id_t &dest,
        const raft_network_session_id_t &session,
        const raft_rpc_request_t<state_t> &request,
        signal_t *interruptor,
        raft_rpc_reply_t *reply_out) {
    if (connected_members.get_key(dest) != boost::make_optional(session)) {
        /* The peer is disconnected or the session ID is outdated */
        return false;
    }
    /* Find the given member's mailbox address */
    raft_business_card_t<state_t> bcard = *peers->get_key(dest);
    /* Send message and wait for a reply */
    disconnect_watcher_t watcher(mailbox_manager, bcard.rpc.get_peer());
    cond_t got_reply;
    mailbox_t<void(raft_rpc_reply_t)> reply_mailbox(
        mailbox_manager,
        [&](signal_t *, raft_rpc_reply_t &&reply) {
            *reply_out = reply;
            got_reply.pulse();
        });
    send(mailbox_manager, bcard.rpc, request, reply_mailbox.get_address());
    wait_any_t waiter(&watcher, &got_reply);
    wait_interruptible(&waiter, interruptor);
    return got_reply.is_pulsed();
}

template<class state_t>
watchable_map_t<raft_member_id_t, raft_network_session_id_t> *
        raft_networked_member_t<state_t>::get_connected_members() {
    return &connected_members;
}

template<class state_t>
void raft_networked_member_t<state_t>::on_bcards_change(
        const raft_member_id_t &peer,
        const raft_business_card_t<state_t> *bcard) {
    
}

template<class state_t>
void raft_networked_member_t<state_t>::on_rpc(
        signal_t *interruptor,
        const raft_member_id_t &member_id,
        const raft_rpc_request_t<state_t> &request,
        const mailbox_t<void(raft_rpc_reply_t)>::address_t &reply_addr) {
    raft_rpc_reply_t reply;
    member.on_rpc(request, interruptor, &reply);
    send(mailbox_manager, reply_addr, reply);
}

#endif   /* CLUSTERING_GENERIC_RAFT_NETWORK_TCC_ */

