#define __STDC_LIMIT_MACROS
#include "buffer_cache/alt/page.hpp"

#include <algorithm>

#include "arch/runtime/coroutines.hpp"
#include "concurrency/auto_drainer.hpp"
#include "serializer/serializer.hpp"

// RSI: temporary debugging macro
// #define pagef debugf
#define pagef(...) do { } while (0)

namespace alt {

page_cache_t::page_cache_t(serializer_t *serializer)
    : serializer_(serializer),
      free_list_(serializer),
      drainer_(new auto_drainer_t) {
    {
        on_thread_t thread_switcher(serializer->home_thread());
        reads_io_account.init(serializer->make_io_account(CACHE_READS_IO_PRIORITY));
        writes_io_account.init(serializer->make_io_account(CACHE_WRITES_IO_PRIORITY));
    }
}

page_cache_t::~page_cache_t() {
    drainer_.reset();
    for (auto it = current_pages_.begin(); it != current_pages_.end(); ++it) {
        delete *it;
    }

    {
        /* IO accounts must be destroyed on the thread they were created on */
        on_thread_t thread_switcher(serializer_->home_thread());
        reads_io_account.reset();
        writes_io_account.reset();
    }
}

current_page_t *page_cache_t::page_for_block_id(block_id_t block_id) {
    if (current_pages_.size() <= block_id) {
        current_pages_.resize(block_id + 1, NULL);
    }

    if (current_pages_[block_id] == NULL) {
        current_pages_[block_id] = new current_page_t();
    } else {
        rassert(!current_pages_[block_id]->is_deleted_);
    }

    return current_pages_[block_id];
}

current_page_t *page_cache_t::page_for_new_block_id(block_id_t *block_id_out) {
    block_id_t block_id = free_list_.acquire_block_id();
    if (current_pages_.size() <= block_id) {
        current_pages_.resize(block_id + 1, NULL);
    }
    if (current_pages_[block_id] == NULL) {
        current_pages_[block_id] =
            new current_page_t(serializer_->get_block_size(),
                               serializer_->malloc(),
                               this);
    } else {
        current_pages_[block_id]->make_non_deleted(serializer_->get_block_size(),
                                                   serializer_->malloc(),
                                                   this);
    }

    *block_id_out = block_id;
    return current_pages_[block_id];
}

struct current_page_help_t {
    current_page_help_t(block_id_t _block_id, page_cache_t *_page_cache)
        : block_id(_block_id), page_cache(_page_cache) { }
    block_id_t block_id;
    page_cache_t *page_cache;
};

current_page_acq_t::current_page_acq_t()
    : txn_(NULL) { }

current_page_acq_t::current_page_acq_t(page_txn_t *txn,
                                       block_id_t block_id,
                                       alt_access_t access)
    : txn_(NULL) {
    init(txn, block_id, access);
}

current_page_acq_t::current_page_acq_t(page_txn_t *txn,
                                       alt_access_t access)
    : txn_(NULL) {
    init(txn, access);
}

void current_page_acq_t::init(page_txn_t *txn,
                              block_id_t block_id,
                              alt_access_t access) {
    guarantee(txn_ == NULL);
    txn_ = txn;
    access_ = access;
    declared_snapshotted_ = false;
    block_id_ = block_id;
    current_page_ = txn->page_cache_->page_for_block_id(block_id);
    dirtied_page_ = false;

    txn_->add_acquirer(this);
    current_page_->add_acquirer(this);
}

void current_page_acq_t::init(page_txn_t *txn,
                              alt_access_t access) {
    guarantee(txn_ == NULL);
    txn_ = txn;
    access_ = access;
    declared_snapshotted_ = false;
    current_page_ = txn->page_cache_->page_for_new_block_id(&block_id_);
    dirtied_page_ = false;
    rassert(access == alt_access_t::write);

    txn_->add_acquirer(this);
    current_page_->add_acquirer(this);
}

current_page_acq_t::~current_page_acq_t() {
    txn_->remove_acquirer(this);
    if (current_page_ != NULL) {
        current_page_->remove_acquirer(this);
    }
}

void current_page_acq_t::declare_readonly() {
    access_ = alt_access_t::read;
    if (current_page_ != NULL) {
        current_page_->pulse_pulsables(this);
    }
}

void current_page_acq_t::declare_snapshotted() {
    rassert(access_ == alt_access_t::read);

    // Allow redeclaration of snapshottedness.
    if (!declared_snapshotted_) {
        declared_snapshotted_ = true;
        rassert(current_page_ != NULL);
        current_page_->pulse_pulsables(this);
    }
}

signal_t *current_page_acq_t::read_acq_signal() {
    return &read_cond_;
}

signal_t *current_page_acq_t::write_acq_signal() {
    rassert(access_ == alt_access_t::write);
    return &write_cond_;
}

page_t *current_page_acq_t::current_page_for_read() {
    rassert(snapshotted_page_.has() || current_page_ != NULL);
    read_cond_.wait();
    if (snapshotted_page_.has()) {
        return snapshotted_page_.get_page_for_read();
    }
    rassert(current_page_ != NULL);
    return current_page_->the_page_for_read(help());
}

page_t *current_page_acq_t::current_page_for_write() {
    rassert(access_ == alt_access_t::write);
    rassert(current_page_ != NULL);
    write_cond_.wait();
    rassert(current_page_ != NULL);
    dirtied_page_ = true;
    return current_page_->the_page_for_write(help());
}

void current_page_acq_t::mark_deleted() {
    rassert(access_ == alt_access_t::write);
    rassert(current_page_ != NULL);
    write_cond_.wait();
    rassert(current_page_ != NULL);
    dirtied_page_ = true;
    current_page_->mark_deleted();
}

bool current_page_acq_t::dirtied_page() const {
    return dirtied_page_;
}

page_cache_t *current_page_acq_t::page_cache() const {
    return txn_->page_cache_;
}

current_page_help_t current_page_acq_t::help() const {
    return current_page_help_t(block_id(), page_cache());
}

current_page_t::current_page_t()
    : is_deleted_(false),
      last_modifier_(NULL) {
}

current_page_t::current_page_t(block_size_t block_size,
                               scoped_malloc_t<ser_buffer_t> buf,
                               page_cache_t *page_cache)
    : page_(new page_t(block_size, std::move(buf), page_cache), page_cache),
      is_deleted_(false),
      last_modifier_(NULL) {
}

current_page_t::~current_page_t() {
    rassert(acquirers_.empty());
    rassert(last_modifier_ == NULL);
}

void current_page_t::make_non_deleted(block_size_t block_size,
                                      scoped_malloc_t<ser_buffer_t> buf,
                                      page_cache_t *page_cache) {
    rassert(is_deleted_);
    rassert(!page_.has());
    is_deleted_ = false;
    page_.init(new page_t(block_size, std::move(buf), page_cache), page_cache);
}

void current_page_t::add_acquirer(current_page_acq_t *acq) {
    acquirers_.push_back(acq);
    pulse_pulsables(acq);
}

void current_page_t::remove_acquirer(current_page_acq_t *acq) {
    current_page_acq_t *next = acquirers_.next(acq);
    acquirers_.remove(acq);
    if (next != NULL) {
        pulse_pulsables(next);
    } else {
        if (is_deleted_) {
            acq->page_cache()->free_list()->release_block_id(acq->block_id());
        }
    }
}

void current_page_t::pulse_pulsables(current_page_acq_t *const acq) {
    const current_page_help_t help = acq->help();

    // First, avoid pulsing when there's nothing to pulse.
    {
        current_page_acq_t *prev = acquirers_.prev(acq);
        if (!(prev == NULL || (prev->access_ == alt_access_t::read
                               && prev->read_cond_.is_pulsed()))) {
            return;
        }
    }

    // Second, avoid re-pulsing already-pulsed chains.
    if (acq->access_ == alt_access_t::read && acq->read_cond_.is_pulsed()
        && !acq->declared_snapshotted_) {
        return;
    }

    // It's time to pulse the pulsables.
    current_page_acq_t *cur = acq;
    while (cur != NULL) {
        // We know that the previous node has read access and has been pulsed as
        // readable, so we pulse the current node as readable.
        cur->read_cond_.pulse_if_not_already_pulsed();

        if (cur->access_ == alt_access_t::read) {
            current_page_acq_t *next = acquirers_.next(cur);
            if (cur->declared_snapshotted_) {
                // Snapshotters get kicked out of the queue, to make way for
                // write-acquirers.

                // We treat deleted pages this way because a write-acquirer may
                // downgrade itself to readonly and snapshotted for the sake of
                // flushing its version of the page -- and if it deleted the page,
                // this is how it learns.
                cur->snapshotted_page_.init(the_page_for_read_or_deleted(help),
                                            cur->page_cache());
                cur->current_page_ = NULL;
                acquirers_.remove(cur);
                // RSI: Dedup this with remove_acquirer.
                if (is_deleted_) {
                    cur->page_cache()->free_list()->release_block_id(acq->block_id());
                }
            }
            cur = next;
        } else {
            // Even the first write-acquirer gets read access (there's no need for an
            // "intent" mode).  But subsequent acquirers need to wait, because the
            // write-acquirer might modify the value.
            if (acquirers_.prev(cur) == NULL) {
                // (It gets exclusive write access if there's no preceding reader.)
                if (is_deleted_) {
                    // Also, if the block is in an "is_deleted_" state right now, we
                    // need to put it into a non-deleted state.  We initialize the
                    // page to a full-sized page.
                    // TODO: We should consider whether we really want this behavior.
                    page_.init(new page_t(help.page_cache->serializer()->get_block_size(),
                                          help.page_cache->serializer()->malloc(),
                                          help.page_cache),
                               help.page_cache);
                    is_deleted_ = false;
                }
                cur->write_cond_.pulse_if_not_already_pulsed();
            }
            break;
        }
    }
}

void current_page_t::mark_deleted() {
    rassert(!is_deleted_);
    is_deleted_ = true;
    page_.reset();
}

void current_page_t::convert_from_serializer_if_necessary(current_page_help_t help) {
    rassert(!is_deleted_);
    if (!page_.has()) {
        page_.init(new page_t(help.block_id, help.page_cache), help.page_cache);
    }
}

page_t *current_page_t::the_page_for_read(current_page_help_t help) {
    rassert(!is_deleted_);
    convert_from_serializer_if_necessary(help);
    return page_.get_page_for_read();
}

page_t *current_page_t::the_page_for_read_or_deleted(current_page_help_t help) {
    if (is_deleted_) {
        return NULL;
    } else {
        return the_page_for_read(help);
    }
}

page_t *current_page_t::the_page_for_write(current_page_help_t help) {
    rassert(!is_deleted_);
    convert_from_serializer_if_necessary(help);
    return page_.get_page_for_write(help.page_cache);
}

page_txn_t *current_page_t::change_last_modifier(page_txn_t *new_last_modifier) {
    rassert(new_last_modifier != NULL);
    page_txn_t *ret = last_modifier_;
    last_modifier_ = new_last_modifier;
    return ret;
}

page_t::page_t(block_id_t block_id, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(0),
      snapshot_refcount_(0) {
    page_cache->evicter().add_not_yet_loaded(this);
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_with_block_id,
                                            this,
                                            block_id,
                                            page_cache));
}

page_t::page_t(block_size_t block_size, scoped_malloc_t<ser_buffer_t> buf,
               page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(block_size.ser_value()),
      buf_(std::move(buf)),
      snapshot_refcount_(0) {
    rassert(buf_.has());
    page_cache->evicter().add_to_evictable_unbacked(this);
}

page_t::page_t(page_t *copyee, page_cache_t *page_cache)
    : destroy_ptr_(NULL),
      ser_buf_size_(0),
      snapshot_refcount_(0) {
    page_cache->evicter().add_not_yet_loaded(this);
    coro_t::spawn_now_dangerously(std::bind(&page_t::load_from_copyee,
                                            this,
                                            copyee,
                                            page_cache));
}

page_t::~page_t() {
    if (destroy_ptr_ != NULL) {
        *destroy_ptr_ = true;
    }
}

void page_t::load_from_copyee(page_t *page, page_t *copyee,
                              page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_ and do some other things.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    auto_drainer_t::lock_t lock(page_cache->drainer_.get());
    page_ptr_t copyee_ptr(copyee, page_cache);

    // Okay, it's safe to block.
    {
        page_acq_t acq;
        acq.init(copyee, page_cache);
        acq.buf_ready_signal()->wait();

        ASSERT_FINITE_CORO_WAITING;
        if (!page_destroyed) {
            // RSP: If somehow there are no snapshotters of copyee now (besides
            // ourself), maybe we could avoid copying this memory.  We need to
            // carefully track snapshotters anyway, once we're comfortable with that,
            // we could do it.

            uint32_t ser_buf_size = copyee->ser_buf_size_;
            rassert(copyee->buf_.has());
            scoped_malloc_t<ser_buffer_t> buf = page_cache->serializer_->malloc();

            memcpy(buf.get(), copyee->buf_.get(), ser_buf_size);

            page->ser_buf_size_ = ser_buf_size;
            page->buf_ = std::move(buf);
            page->destroy_ptr_ = NULL;

            page->pulse_waiters_or_make_evictable(page_cache);
        }
    }
}


void page_t::load_with_block_id(page_t *page, block_id_t block_id,
                                page_cache_t *page_cache) {
    // This is called using spawn_now_dangerously.  We need to atomically set
    // destroy_ptr_.
    bool page_destroyed = false;
    rassert(page->destroy_ptr_ == NULL);
    page->destroy_ptr_ = &page_destroyed;

    auto_drainer_t::lock_t lock(page_cache->drainer_.get());

    scoped_malloc_t<ser_buffer_t> buf;
    counted_t<standard_block_token_t> block_token;
    {
        serializer_t *const serializer = page_cache->serializer_;
        on_thread_t th(serializer->home_thread());
        block_token = serializer->index_read(block_id);
        rassert(block_token.has());
        buf = serializer->malloc();
        serializer->block_read(block_token,
                               buf.get(),
                               page_cache->reads_io_account.get());
    }

    ASSERT_FINITE_CORO_WAITING;
    if (page_destroyed) {
        return;
    }

    rassert(!page->block_token_.has());
    rassert(!page->buf_.has());
    rassert(block_token.has());
    page->ser_buf_size_ = block_token->block_size().ser_value();
    page->buf_ = std::move(buf);
    page->block_token_ = std::move(block_token);

    page->pulse_waiters_or_make_evictable(page_cache);
}

void page_t::add_snapshotter() {
    // This may not block, because it's called at the beginning of
    // page_t::load_from_copyee.
    ASSERT_NO_CORO_WAITING;
    ++snapshot_refcount_;
}

void page_t::remove_snapshotter(page_cache_t *page_cache) {
    rassert(snapshot_refcount_ > 0);
    --snapshot_refcount_;
    if (snapshot_refcount_ == 0) {
        // Every page_acq_t is bounded by the lifetime of some page_ptr_t: either the
        // one in current_page_acq_t or its current_page_t or the one in
        // load_from_copyee.
        rassert(waiters_.empty());

        page_cache->evicter().remove_page(this);
        delete this;
    }
}

size_t page_t::num_snapshot_references() {
    return snapshot_refcount_;
}

page_t *page_t::make_copy(page_cache_t *page_cache) {
    page_t *ret = new page_t(this, page_cache);
    return ret;
}

void page_t::pulse_waiters_or_make_evictable(page_cache_t *page_cache) {
    rassert(page_cache->evicter().page_is_in_unevictable_bag(this));
    page_cache->evicter().add_now_loaded_size(ser_buf_size_);
    if (waiters_.empty()) {
        page_cache->evicter().move_unevictable_to_evictable(this);
    } else {
        for (page_acq_t *p = waiters_.head(); p != NULL; p = waiters_.next(p)) {
            // The waiter's not already going to have been pulsed.
            p->buf_ready_signal_.pulse();
        }
    }
}

void page_t::add_waiter(page_acq_t *acq) {
    eviction_bag_t *old_bag
        = acq->page_cache()->evicter().correct_eviction_category(this);
    waiters_.push_back(acq);
    acq->page_cache()->evicter().change_eviction_bag(old_bag, this);
    if (buf_.has()) {
        acq->buf_ready_signal_.pulse();
    }
}

uint32_t page_t::get_page_buf_size() {
    rassert(buf_.has());
    rassert(ser_buf_size_ != 0);
    return block_size_t::unsafe_make(ser_buf_size_).value();
}

void *page_t::get_page_buf() {
    rassert(buf_.has());
    return buf_->cache_data;
}

void page_t::reset_block_token() {
    // The page is supposed to have its buffer acquired in reset_block_token -- it's
    // the thing modifying the page.  We thus assume that the page is unevictable and
    // resetting block_token_ doesn't change that.
    rassert(!waiters_.empty());
    block_token_.reset();
}


void page_t::remove_waiter(page_acq_t *acq) {
    eviction_bag_t *old_bag
        = acq->page_cache()->evicter().correct_eviction_category(this);
    waiters_.remove(acq);
    acq->page_cache()->evicter().change_eviction_bag(old_bag, this);

    // page_acq_t always has a lesser lifetime than some page_ptr_t.
    rassert(snapshot_refcount_ > 0);
}

page_acq_t::page_acq_t() : page_(NULL), page_cache_(NULL) {
}

void page_acq_t::init(page_t *page, page_cache_t *page_cache) {
    rassert(page_ == NULL);
    rassert(page_cache_ == NULL);
    rassert(!buf_ready_signal_.is_pulsed());
    page_ = page;
    page_cache_ = page_cache;
    page_->add_waiter(this);
}

page_acq_t::~page_acq_t() {
    if (page_ != NULL) {
        rassert(page_cache_ != NULL);
        page_->remove_waiter(this);
    }
}

bool page_acq_t::has() const {
    return page_ != NULL;
}

signal_t *page_acq_t::buf_ready_signal() {
    return &buf_ready_signal_;
}

uint32_t page_acq_t::get_buf_size() {
    buf_ready_signal_.wait();
    return page_->get_page_buf_size();
}

void *page_acq_t::get_buf_write() {
    buf_ready_signal_.wait();
    page_->reset_block_token();
    return page_->get_page_buf();
}

const void *page_acq_t::get_buf_read() {
    buf_ready_signal_.wait();
    return page_->get_page_buf();
}

free_list_t::free_list_t(serializer_t *serializer) {
    on_thread_t th(serializer->home_thread());

    next_new_block_id_ = serializer->max_block_id();
    for (block_id_t i = 0; i < next_new_block_id_; ++i) {
        if (serializer->get_delete_bit(i)) {
            free_ids_.push_back(i);
        }
    }
}

free_list_t::~free_list_t() { }

block_id_t free_list_t::acquire_block_id() {
    if (free_ids_.empty()) {
        block_id_t ret = next_new_block_id_;
        ++next_new_block_id_;
        return ret;
    } else {
        block_id_t ret = free_ids_.back();
        free_ids_.pop_back();
        return ret;
    }
}

void free_list_t::release_block_id(block_id_t block_id) {
    free_ids_.push_back(block_id);
}

page_ptr_t::page_ptr_t() : page_(NULL), page_cache_(NULL) {
}

page_ptr_t::~page_ptr_t() {
    reset();
}

page_ptr_t::page_ptr_t(page_ptr_t &&movee)
    : page_(movee.page_), page_cache_(movee.page_cache_) {
    movee.page_ = NULL;
    movee.page_cache_ = NULL;
}

page_ptr_t &page_ptr_t::operator=(page_ptr_t &&movee) {
    page_ptr_t tmp(std::move(movee));
    std::swap(page_, tmp.page_);
    std::swap(page_cache_, tmp.page_cache_);
    return *this;
}

void page_ptr_t::init(page_t *page, page_cache_t *page_cache) {
    rassert(page_ == NULL && page_cache_ == NULL);
    page_ = page;
    page_cache_ = page_cache;
    if (page_ != NULL) {
        page_->add_snapshotter();
    }
}

void page_ptr_t::reset() {
    if (page_ != NULL) {
        page_t *ptr = page_;
        page_cache_t *cache = page_cache_;
        page_ = NULL;
        page_cache_ = NULL;
        ptr->remove_snapshotter(cache);
    }
}

page_t *page_ptr_t::get_page_for_read() {
    rassert(page_ != NULL);
    return page_;
}

page_t *page_ptr_t::get_page_for_write(page_cache_t *page_cache) {
    rassert(page_ != NULL);
    if (page_->num_snapshot_references() > 1) {
        page_ptr_t tmp(page_->make_copy(page_cache), page_cache);
        *this = std::move(tmp);
    }
    return page_;
}

page_txn_t::page_txn_t(page_cache_t *page_cache, page_txn_t *preceding_txn_or_null)
    : page_cache_(page_cache),
      began_waiting_for_flush_(false) {
    if (preceding_txn_or_null != NULL) {
        connect_preceder(preceding_txn_or_null);
    }
}

void page_txn_t::connect_preceder(page_txn_t *preceder) {
    if (!preceder->flush_complete_cond_.is_pulsed()) {
        // RSP: performance
        if (std::find(preceders_.begin(), preceders_.end(), preceder)
            == preceders_.end()) {
            preceders_.push_back(preceder);
            preceder->subseqers_.push_back(this);
        }
    }
}

void page_txn_t::remove_preceder(page_txn_t *preceder) {
    auto it = std::find(preceders_.begin(), preceders_.end(), preceder);
    rassert(it != preceders_.end());
    preceders_.erase(it);

}

page_txn_t::~page_txn_t() {
    rassert(live_acqs_.empty(), "current_page_acq_t lifespan exceeds its page_txn_t's");

    // RSI: Remove this assertion when we support manually starting txn flushes
    // sooner.
    rassert(!began_waiting_for_flush_);

    if (!began_waiting_for_flush_) {
        pagef("in ~page_txn_t, going to announce waiting for flush\n");
        announce_waiting_for_flush_if_we_should();
    }

    // RSI: Do we want to wait for this here?  Or should the page_cache_t be the
    // thing that waits and destroys this object?

    // RSI: Do whatever else is necessary to implement this.

    pagef("in ~page_txn_t, waiting for flush cond\n");
    flush_complete_cond_.wait();
    pagef("in ~page_txn_t, flush cond complete\n");
}

void page_txn_t::add_acquirer(current_page_acq_t *acq) {
    live_acqs_.push_back(acq);
}

void page_txn_t::remove_acquirer(current_page_acq_t *acq) {
    // This is called by acq's destructor.
    {
        auto it = std::find(live_acqs_.begin(), live_acqs_.end(), acq);
        rassert(it != live_acqs_.end());
        live_acqs_.erase(it);
    }

    // We check if acq->read_cond_.is_pulsed() so that if we delete the acquirer
    // before we got any kind of access to the block, then we can't have dirtied the
    // page or touched the page.

    if (acq->read_cond_.is_pulsed() && acq->access_ == alt_access_t::write) {
        // It's not snapshotted because you can't snapshot write acqs.  (We
        // rely on this fact solely because we need to grab the block_id_t
        // and current_page_acq_t currently doesn't know it.)
        rassert(acq->current_page_ != NULL);

        // Get the block id while current_page_ is non-null.  (It'll become
        // null once we're snapshotted.)
        const block_id_t block_id = acq->block_id();

        if (acq->dirtied_page()) {
            // We know we hold an exclusive lock.
            rassert(acq->write_cond_.is_pulsed());

            // Set the last modifier while current_page_ is non-null (and while we're the
            // exclusive holder).
            {
                page_txn_t *const previous_modifier
                    = acq->current_page_->change_last_modifier(this);

                // RSP: Performance (in the assertion).
                rassert(pages_modified_last_.end()
                        == std::find(pages_modified_last_.begin(),
                                     pages_modified_last_.end(),
                                     acq->current_page_));
                pages_modified_last_.push_back(acq->current_page_);

                if (previous_modifier != NULL) {
                    auto it = std::find(previous_modifier->pages_modified_last_.begin(),
                                        previous_modifier->pages_modified_last_.end(),
                                        acq->current_page_);
                    rassert(it != previous_modifier->pages_modified_last_.end());
                    previous_modifier->pages_modified_last_.erase(it);

                    connect_preceder(previous_modifier);
                }
            }


            // Declare readonly (so that we may declare acq snapshotted).
            acq->declare_readonly();
            acq->declare_snapshotted();

            // Since we snapshotted the lead acquirer, it gets detached.
            rassert(acq->current_page_ == NULL);
            // Steal the snapshotted page_ptr_t.
            page_ptr_t local = std::move(acq->snapshotted_page_);
            snapshotted_dirtied_pages_.push_back(dirtied_page_t(block_id,
                                                                std::move(local),
                                                                repli_timestamp_t::invalid /* RSI: handle recency */));
        } else {
            touched_pages_.push_back(std::make_pair(block_id, repli_timestamp_t::invalid /* RSI: handle recency */));
        }
    }
}

void page_txn_t::announce_waiting_for_flush_if_we_should() {
    if (live_acqs_.empty()) {
        rassert(!began_waiting_for_flush_);
        began_waiting_for_flush_ = true;
        page_cache_->im_waiting_for_flush(this);
    }
}

struct block_token_tstamp_t {
    block_token_tstamp_t(block_id_t _block_id,
                         bool _is_deleted,
                         counted_t<standard_block_token_t> _block_token,
                         repli_timestamp_t _tstamp)
        : block_id(_block_id), is_deleted(_is_deleted),
          block_token(std::move(_block_token)), tstamp(_tstamp) { }
    block_id_t block_id;
    bool is_deleted;
    counted_t<standard_block_token_t> block_token;
    repli_timestamp_t tstamp;
};

void page_cache_t::do_flush_txn(page_cache_t *page_cache, page_txn_t *txn) {
    pagef("do_flush_txn (pc=%p, txn=%p)\n", page_cache, txn);
    // We're going to flush this transaction.  Let's start its flush, then detach
    // this transaction from its subseqers, then notify its subseqers that
    // they've lost a preceder.

    // RSI: We shouldn't go through this rigamarole when touched_pages_ and
    // snapshotted_dirty_pages_ is empty (i.e. for read transactions or write
    // transactions that didn't do anything).

    // RSP: This implementation is fine, but the strategy of having each txn
    // snapshot and be flushed independently is suboptimal.

    // RSP: reserve blocks_by_tokens.
    std::vector<block_token_tstamp_t> blocks_by_tokens;
    blocks_by_tokens.reserve(txn->touched_pages_.size()
                             + txn->snapshotted_dirtied_pages_.size());

    std::vector<std::pair<block_id_t, repli_timestamp_t> > ancillary_infos;
    std::vector<buf_write_info_t> write_infos;
    write_infos.reserve(txn->touched_pages_.size()
                        + txn->snapshotted_dirtied_pages_.size());
    ancillary_infos.reserve(txn->touched_pages_.size()
                            + txn->snapshotted_dirtied_pages_.size());

    for (size_t i = 0, e = txn->snapshotted_dirtied_pages_.size(); i < e; ++i) {
        page_txn_t::dirtied_page_t *dp = &txn->snapshotted_dirtied_pages_[i];
        if (!dp->ptr.has()) {
            // The page is deleted.
            blocks_by_tokens.push_back(block_token_tstamp_t(dp->block_id,
                                                            true,
                                                            counted_t<standard_block_token_t>(),
                                                            dp->tstamp));
        } else {
            page_t *page = dp->ptr.get_page_for_read();

            // If it's already on disk, we're not going to flush it.
            if (page->block_token_.has()) {
                blocks_by_tokens.push_back(block_token_tstamp_t(dp->block_id,
                                                                false,
                                                                page->block_token_,
                                                                dp->tstamp));
            } else {
                // We can't be in the process of loading a block we're going to write
                // that we don't have a block token for.  That's because we _actually
                // dirtied the page_.  We had to have acquired the buf, and the only
                // way to get rid of the buf is for it to be evicted, in which case
                // the block token would be non-empty.
                rassert(page->destroy_ptr_ == NULL);

                rassert(page->buf_.has());

                // RSI: Is there a page_acq_t for this buf we're writing?  There had
                // better be.
                write_infos.push_back(buf_write_info_t(page->buf_.get(),
                                                       block_size_t::unsafe_make(page->ser_buf_size_),
                                                       dp->block_id));
                ancillary_infos.push_back(std::make_pair(dp->block_id, dp->tstamp));
            }
        }
    }

    for (size_t i = 0, e = txn->touched_pages_.size(); i < e; ++i) {
        // "is_deleted == false and !block_token.has()" means the page is just
        // touched.
        blocks_by_tokens.push_back(block_token_tstamp_t(txn->touched_pages_[i].first,
                                                        false,
                                                        counted_t<standard_block_token_t>(),
                                                        txn->touched_pages_[i].second));
    }

    // RSI: Take the newly written blocks' block tokens and set their page_t's block
    // token field to them.
    {
        pagef("do_flush_txn about to thread switch (pc=%p, txn=%p)\n", page_cache, txn);

        on_thread_t th(page_cache->serializer_->home_thread());

        pagef("do_flush_txn switched threads (pc=%p, txn=%p)\n", page_cache, txn);
        struct : public iocallback_t, public cond_t {
            void on_io_complete() {
                pulse();
            }
        } blocks_releasable_cb;
        std::vector<counted_t<standard_block_token_t> > tokens
            = page_cache->serializer_->block_writes(write_infos,
                                                    page_cache->writes_io_account.get(),
                                                    &blocks_releasable_cb);

        rassert(tokens.size() == write_infos.size());
        rassert(write_infos.size() == ancillary_infos.size());
        for (size_t i = 0; i < write_infos.size(); ++i) {
            blocks_by_tokens.push_back(block_token_tstamp_t(ancillary_infos[i].first,
                                                            false,
                                                            std::move(tokens[i]),
                                                            ancillary_infos[i].second));
        }

        // RSP: Unnecessary copying between blocks_by_tokens and write_ops, inelegant
        // representation of deletion/touched blocks in blocks_by_tokens.
        std::vector<index_write_op_t> write_ops;
        write_ops.reserve(blocks_by_tokens.size());

        for (auto it = blocks_by_tokens.begin(); it != blocks_by_tokens.end();
             ++it) {
            if (it->is_deleted) {
                write_ops.push_back(index_write_op_t(it->block_id,
                                                     counted_t<standard_block_token_t>(),
                                                     repli_timestamp_t::invalid));
            } else if (it->block_token.has()) {
                write_ops.push_back(index_write_op_t(it->block_id,
                                                     std::move(it->block_token),
                                                     it->tstamp));
            } else {
                write_ops.push_back(index_write_op_t(it->block_id,
                                                     boost::none,
                                                     it->tstamp));
            }
        }

        pagef("do_flush_txn blocks_releasable_cb.wait() (pc=%p, txn=%p)\n", page_cache, txn);

        blocks_releasable_cb.wait();

        pagef("do_flush_txn blocks_releasable_cb waited (pc=%p, txn=%p)\n", page_cache, txn);

        // RSI: This blocks?  Is there any way to set the began_index_write_
        // field?
        page_cache->serializer_->index_write(write_ops,
                                             page_cache->writes_io_account.get());
        pagef("do_flush_txn index write returned (pc=%p, txn=%p)\n", page_cache, txn);
    }

    // Flush complete, and we're back on the page cache's thread.

    // RSI: connect_preceder uses flush_complete_cond_ to see whether it should
    // connect.  It should probably use began_index_write_, when that variable
    // exists.
    ASSERT_NO_CORO_WAITING;
    std::vector<page_txn_t *> subseqers = std::move(txn->subseqers_);
    txn->subseqers_.clear();

    for (auto it = subseqers.begin(); it != subseqers.end(); ++it) {
        (*it)->remove_preceder(txn);
        // Flush subseqers that are ready to go.
        if ((*it)->began_waiting_for_flush_) {
            page_cache->im_waiting_for_flush(*it);
        }
    }

    for (auto it = txn->pages_modified_last_.begin();
         it != txn->pages_modified_last_.end();
         ++it) {
        current_page_t *current_page = *it;
        rassert(current_page->last_modifier_ == txn);
        current_page->last_modifier_ = NULL;
    }
    txn->pages_modified_last_.clear();

    txn->flush_complete_cond_.pulse();
}

void page_cache_t::im_waiting_for_flush(page_txn_t *txn) {
    pagef("im_waiting_for_flush (txn=%p)\n", txn);
    rassert(txn->began_waiting_for_flush_);
    // rassert(!txn->began_index_write_);  // RSI: This variable doesn't exist.
    rassert(txn->live_acqs_.empty());

    // This txn is now waiting to be flushed.  Should we flush it?  Let's look at the
    // graph of txns.  We may flush this txn if all its preceding txns can be
    // flushed.

    if (txn->preceders_.empty()) {
        pagef("preceders empty, flushing (txn=%p).\n", txn);

        // RSI: 'ordered'?  Really?
        coro_t::spawn_later_ordered(std::bind(&page_cache_t::do_flush_txn, this, txn));
    }
}

eviction_bag_t::eviction_bag_t()
    : bag_(&page_t::eviction_index), size_(0) { }

eviction_bag_t::~eviction_bag_t() {
    guarantee(bag_.size() == 0);
    guarantee(size_ == 0);
}

void eviction_bag_t::add_without_size(page_t *page) {
    bag_.add(page);
}

void eviction_bag_t::add_size(uint32_t ser_buf_size) {
    size_ += ser_buf_size;
}

void eviction_bag_t::add(page_t *page, uint32_t ser_buf_size) {
    bag_.add(page);
    size_ += ser_buf_size;
}

void eviction_bag_t::remove(page_t *page, uint32_t ser_buf_size) {
    bag_.remove(page);
    uint64_t value = ser_buf_size;
    rassert(value <= size_, "value = %" PRIu64 ", size_ = %" PRIu64,
            value, size_);
    size_ -= value;
}

bool eviction_bag_t::has_page(page_t *page) const {
    return bag_.has_element(page);
}

evicter_t::evicter_t() { }

evicter_t::~evicter_t() { }


void evicter_t::add_not_yet_loaded(page_t *page) {
    unevictable_.add_without_size(page);
}

void evicter_t::add_now_loaded_size(uint32_t ser_buf_size) {
    unevictable_.add_size(ser_buf_size);
}

bool evicter_t::page_is_in_unevictable_bag(page_t *page) const {
    return unevictable_.has_page(page);
}

void evicter_t::add_to_evictable_unbacked(page_t *page) {
    evictable_unbacked_.add(page, page->ser_buf_size_);
}

void evicter_t::move_unevictable_to_evictable(page_t *page) {
    rassert(unevictable_.has_page(page));
    unevictable_.remove(page, page->ser_buf_size_);
    eviction_bag_t *new_bag = correct_eviction_category(page);
    rassert(new_bag == &evictable_disk_backed_
            || new_bag == &evictable_unbacked_);
    new_bag->add(page, page->ser_buf_size_);
}

void evicter_t::change_eviction_bag(eviction_bag_t *current_bag,
                                    page_t *page) {
    rassert(current_bag->has_page(page));
    current_bag->remove(page, page->ser_buf_size_);
    eviction_bag_t *new_bag = correct_eviction_category(page);
    new_bag->add(page, page->ser_buf_size_);
}

eviction_bag_t *evicter_t::correct_eviction_category(page_t *page) {
    if (page->destroy_ptr_ != NULL || !page->waiters_.empty()) {
        return &unevictable_;
    } else if (!page->buf_.has()) {
        return &evicted_;
    } else if (page->block_token_.has()) {
        return &evictable_disk_backed_;
    } else {
        return &evictable_unbacked_;
    }
}

void evicter_t::remove_page(page_t *page) {
    rassert(page->waiters_.empty());
    rassert(page->snapshot_refcount_ == 0);
    eviction_bag_t *bag = correct_eviction_category(page);
    bag->remove(page, page->ser_buf_size_);
}



}  // namespace alt

