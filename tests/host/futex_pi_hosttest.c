/* PI futex operations on the real futex code over modeled user pages.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c rach2/sys/tick.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 */
#define main ordinary_futex_rows
#include "futex_hosttest.c"
#undef main

static process_t owner, high, low;
static struct mm pi_mm;
static struct futex_q high_q, low_q;
static struct futex_sleeper high_s, low_s;

/** Reset all queues and give four threads one address space. */
static void pi_reset(void)
{
    k_memset(_waiters, 0, sizeof(_waiters));
    k_memset(user, 0, sizeof(user));
    k_memset(&self, 0, sizeof(self));
    k_memset(&owner, 0, sizeof(owner));
    k_memset(&high, 0, sizeof(high));
    k_memset(&low, 0, sizeof(low));
    self.pid = 3;
    owner.pid = 5;
    high.pid = 7;
    low.pid = 8;
    self.mm = owner.mm = high.mm = low.mm = &pi_mm;
    self.page_dir = owner.page_dir = high.page_dir = low.page_dir = (page_dir_t *)16;
    self.state = owner.state = high.state = low.state = PROC_RUNNING;
    owner.next = &high;
    high.next = &low;
    named = &owner;
    cpu_table[0].current = &self;
    kernel_ticks = 100;
    tick_pending = slow_at = raced_at = waiters_at = 0;
}

/** Attach a modeled sleeping task and return its PI state. */
static struct futex_pi *pi_file(process_t *task, struct futex_q *q, struct futex_sleeper *s,
                                uintptr_t addr)
{
    struct futex_pi *spare = kmalloc(sizeof(*spare));
    int rc;

    k_memset(q, 0, sizeof(*q));
    k_memset(s, 0, sizeof(*s));
    q->target_space = self.page_dir;
    q->target_key = addr;
    q->task = task;
    q->bitset = FUTEX_MATCH_ANY;
    q->s = s;
    rc = pi_attach(addr, q, &spare, 0, 0);
    CHECK(rc == 0, "attach contended PI waiter");
    kfree(spare);
    if (rc == 0) {
        futex_enqueue(q);
        futex_pi_recalculate();
    }
    return q->pi;
}

/** Remove a synthetic waiter and its reference after its test. */
static void pi_unfile(struct futex_q *q)
{
    futex_unlink(q, futex_bucket(q->space, q->uaddr));
    q->task->pi_blocked = NULL;
    pi_put(q->pi);
    futex_pi_recalculate();
}

/** Basic Linux PI operation results and word transitions. */
static void pi_basic_rows(void)
{
    pi_reset();
    CHECK(futex_lock_pi(32, NULL, FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG, 0) == 0,
          "LOCK_PI acquires a zero word");
    CHECK(word_at(32) == 3, "acquisition publishes owner TID");
    CHECK(futex_lock_pi(32, NULL, FUTEX_TRYLOCK_PI | FUTEX_PRIVATE_FLAG, 1) == -EDEADLK,
          "recursive trylock is EDEADLK");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0 && word_at(32) == 0,
          "uncontended unlock clears the word");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == -EPERM, "unowned unlock is EPERM");
    word_set(32, 99);
    CHECK(futex_lock_pi(32, NULL, FUTEX_PRIVATE_FLAG, 1) == -ESRCH, "a missing owner is ESRCH");
    word_set(32, owner.pid);
    CHECK(futex_lock_pi(32, NULL, FUTEX_PRIVATE_FLAG, 1) == -EAGAIN, "contended trylock is EAGAIN");
    CHECK((word_at(32) & FUTEX_WAITERS) != 0, "failed trylock sets WAITERS");
    CHECK(futex_lock_pi(32, NULL, FUTEX_PRIVATE_FLAG, 0) == -ETIMEDOUT,
          "modeled wait returns its timeout");
    CHECK(pi_states == NULL && self.pi_blocked == NULL, "timeout removes PI state");
    word_set(32, FUTEX_OWNER_DIED);
    CHECK(futex_lock_pi(32, NULL, FUTEX_PRIVATE_FLAG, 0) == 0 &&
              word_at(32) == (3 | FUTEX_OWNER_DIED),
          "takeover preserves OWNER_DIED");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "unlock takeover");
    word_set(32, 0);
    raced_at = 32;
    CHECK(futex_lock_pi(32, NULL, FUTEX_PRIVATE_FLAG, 0) == 0,
          "LOCK_PI retries an atomic exchange race");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "unlock raced acquisition");
}

/** Donation, ordered handoff, chain propagation and cycle detection. */
static void pi_priority_rows(void)
{
    struct futex_pi *pi, *second;
    struct futex_q chain_q;
    struct futex_sleeper chain_s;
    struct futex_pi *spare;
    struct futex_q cycle_q;

    pi_reset();
    low.rt_priority = 20;
    high.rt_priority = 80;
    word_set(32, owner.pid);
    pi = pi_file(&low, &low_q, &low_s, 32);
    (void)pi_file(&high, &high_q, &high_s, 32);
    CHECK(owner.pi_priority == 80, "owner runs at top waiter's priority");
    CHECK(owner.rt_priority == 0, "donation leaves the base priority unchanged");
    word_set(36, self.pid);
    second = pi_file(&owner, &chain_q, &chain_s, 36);
    CHECK(self.pi_priority == 80, "priority crosses a blocked-owner chain");
    spare = kmalloc(sizeof(*spare));
    k_memset(&cycle_q, 0, sizeof(cycle_q));
    cycle_q.task = &self;
    cycle_q.target_space = self.page_dir;
    cycle_q.target_key = 32;
    CHECK(pi_attach(32, &cycle_q, &spare, 0, 0) == -EDEADLK, "a cycle across two locks is EDEADLK");
    kfree(spare);
    (void)second;
    pi_unfile(&chain_q);
    CHECK(self.pi_priority == 0, "removing a chain removes its donation");
    cpu_table[0].current = &owner;
    CHECK(futex_wake(32, 1, FUTEX_MATCH_ANY, FUTEX_PRIVATE_FLAG) == -EINVAL,
          "ordinary wake refuses a PI waiter");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "contended owner unlocks");
    CHECK(high_q.woken && !low_q.woken && pi->owner == &high,
          "handoff selects highest priority ahead of FIFO order");
    CHECK(word_at(32) == (FUTEX_WAITERS | high.pid), "handoff publishes owner and WAITERS");
    CHECK(owner.pi_priority == 0, "unlock restores old owner's priority");
    cpu_table[0].current = &high;
    pi_unfile(&high_q);
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "second handoff");
    cpu_table[0].current = &low;
    pi_unfile(&low_q);
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0 && !pi_states,
          "last owner unlocks without retained state");
}

/** A dying owner transfers the lock; the waiter repairs its own mapping. */
static void pi_death_rows(void)
{
    struct futex_pi *pi;

    pi_reset();
    word_set(32, owner.pid);
    pi = pi_file(&self, &high_q, &high_s, 32);
    cpu_table[0].current = &owner;
    proc_thread_gone(&owner);
    CHECK(high_q.woken && pi->owner == &self && pi->died,
          "exit transfers a contended lock even without a robust list");
    cpu_table[0].current = &self;
    CHECK(pi_fixup(32, pi) == 0 && word_at(32) == (3 | FUTEX_WAITERS | FUTEX_OWNER_DIED),
          "new owner repairs OWNER_DIED through its mapping");
    pi_unfile(&high_q);
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "unlock dead owner's lock");
}

/** Transfer the current condition waiter while the modeled wait sleeps. */
static void requeue_step(void)
{
    CHECK(futex_cmp_requeue_pi(64, 1, 0, 32, 9, FUTEX_PRIVATE_FLAG) == 1,
          "requeue acquires a free PI mutex for the waiter");
}

/** The requeue pairing, comparison and error paths. */
static void pi_requeue_rows(void)
{
    pi_reset();
    word_set(64, 9);
    CHECK(futex_cmp_requeue_pi(64, 0, 0, 32, 9, FUTEX_PRIVATE_FLAG) == -EINVAL,
          "requeue requires exactly one wake");
    CHECK(futex_cmp_requeue_pi(64, 1, 0, 32, 8, FUTEX_PRIVATE_FLAG) == -EAGAIN,
          "requeue compares the source word");
    CHECK(futex_wait_requeue_pi(64, 9, NULL, 64, FUTEX_PRIVATE_FLAG) == -EINVAL,
          "requeue source and target must differ");
    pi_wait_step = requeue_step;
    CHECK(futex_wait_requeue_pi(64, 9, NULL, 32, FUTEX_PRIVATE_FLAG) == 0,
          "WAIT_REQUEUE_PI returns owning the target");
    CHECK(word_at(32) == (3 | FUTEX_WAITERS) && !pi_states, "requeue ownership and cleanup");
    CHECK(futex_unlock_pi(32, FUTEX_PRIVATE_FLAG) == 0, "unlock requeued mutex");
    waiter_file(64);
    CHECK(futex_cmp_requeue_pi(64, 1, 0, 32, 9, FUTEX_PRIVATE_FLAG) == -EINVAL,
          "requeue refuses ordinary waiters");
    waiter_unfile();
}

/** Run the PI rows alone; ordinary futex rows have their own executable. */
int main(void)
{
    pi_basic_rows();
    pi_priority_rows();
    pi_death_rows();
    pi_requeue_rows();
    CHECK(!pi_states, "all PI state released");
    if (!failures) {
        printf("futex-pi-test: pass\n");
    }
    return failures != 0;
}
