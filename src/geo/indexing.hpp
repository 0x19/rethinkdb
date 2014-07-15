// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef GEO_INDEXING_HPP_
#define GEO_INDEXING_HPP_

#include <string>
#include <vector>

#include "btree/parallel_traversal.hpp"
#include "containers/counted.hpp"
#include "geo/s2/s2cellid.h"

namespace ql {
class datum_t;
}
class signal_t;


extern const int GEO_INDEX_GOAL_GRID_CELLS;

std::vector<std::string> compute_index_grid_keys(
        const counted_t<const ql::datum_t> &key,
        int goal_cells);

// TODO (daniel): Support compound indexes somehow.
class geo_index_traversal_helper_t : public btree_traversal_helper_t {
public:
    geo_index_traversal_helper_t(const std::vector<std::string> &query_grid_keys);

    /* Called for every pair that could potentially intersect with query_grid_keys.
    Note that this might be called multiple times for the same value. */
    virtual void on_candidate(
            const btree_key_t *key,
            const void *value,
            buf_parent_t parent,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t) = 0;

    /* btree_traversal_helper_t interface */
    void process_a_leaf(
            buf_lock_t *leaf_node_buf,
            const btree_key_t *left_exclusive_or_null,
            const btree_key_t *right_inclusive_or_null,
            signal_t *interruptor,
            int *population_change_out)
            THROWS_ONLY(interrupted_exc_t);
    void postprocess_internal_node(UNUSED buf_lock_t *internal_node_buf) { }
    void filter_interesting_children(
            buf_parent_t parent,
            ranged_block_ids_t *ids_source,
            interesting_children_callback_t *cb);
    access_t btree_superblock_mode() { return access_t::read; }
    access_t btree_node_mode() { return access_t::read; }

protected:
    // Once called, no further calls to on_candidate() will be made and
    // the traversal will be aborted as quickly as possible.
    void abort_traversal();

private:
    static bool cell_intersects_with_range(
            S2CellId c, const S2CellId left_min, const S2CellId right_max);
    bool any_query_cell_intersects(const btree_key_t *left_excl,
                                   const btree_key_t *right_incl);
    bool any_query_cell_intersects(const S2CellId left_min,
                                   const S2CellId right_max);

    std::vector<S2CellId> query_cells_;
    bool abort_;
};

#endif  // GEO_INDEXING_HPP_
