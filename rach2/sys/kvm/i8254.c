/* The guest's 8254 counters, latches, silent speaker and IRQ0 reinjection.
 * The controllers' lock (irq.c) serializes timer checks, port accesses and
 * state ioctls. */

#include "kvm.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

#define PIT_HZ            1193181u
#define PIT_NS            1000000000ull
#define PIT_MIN_PERIOD_NS 200000ull
#define PIT_LSB           1u
#define PIT_MSB           2u
#define PIT_WORD_LOW      3u
#define PIT_WORD_HIGH     4u

struct guest_pit {
    struct kvm_pit_state2 state;
    uint64_t period;  /* host nanoseconds, 0 before a timer is programmed */
    uint64_t expires; /* host deadline, retained after a one-shot expires */
    uint32_t pending; /* ticks accumulated, including the one awaiting EOI */
    int active;       /* a host expiry is armed */
    int periodic;
    int inflight; /* one of `pending` has been delivered */
    int reinject; /* retain ticks until the guest acknowledges them */
    int speaker;  /* KVM_PIT_SPEAKER_DUMMY owns port 0x61 */
};

/** The PIT of a VM, or NULL before either create ioctl. */
static struct guest_pit *pit_of(struct kvm_vm *vm)
{
    return (struct guest_pit *)vm->pit;
}

/** Have every vCPU, a halted one too, look at the timers and find its
 * next deadline again (KVM_REQ_TIMER), since the PIT is the VM's and any
 * vCPU's thread may change it. */
static void pit_wake(struct kvm_vm *vm)
{
    uint32_t i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_lapic_wake(vm->vcpus[i], KVM_REQ_TIMER);
    }
}

/** The counter's elapsed input clocks without overflowing ns times Hz. */
static uint64_t pit_ticks(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];
    uint64_t now = kvm_host_ns(), elapsed, load = (uint64_t)s->count_load_time;

    if (channel == 0) {
        if (!p->period) {
            return 0;
        }
        elapsed =
            now >= p->expires ? p->period + (now - p->expires) : p->period - (p->expires - now);
    } else {
        elapsed = now >= load ? now - load : 0;
    }
    return (elapsed / PIT_NS) * PIT_HZ + (elapsed % PIT_NS) * PIT_HZ / PIT_NS;
}

/** The 16-bit count as Linux's binary counter model exposes it. */
static uint16_t pit_count(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];
    uint64_t ticks = pit_ticks(p, channel);

    switch (s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        return (uint16_t)(s->count - ticks);
    case 3:
        return (uint16_t)(s->count - (ticks * 2u) % s->count);
    default:
        return (uint16_t)(s->count - ticks % s->count);
    }
}

/** OUT for the count's mode, used by read-back and the speaker port. */
static uint32_t pit_out(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];
    uint64_t ticks = pit_ticks(p, channel);

    switch (s->mode) {
    case 1:
        return ticks < s->count;
    case 2:
        return ticks && ticks % s->count == 0;
    case 3:
        return ticks % s->count < (s->count + 1u) / 2u;
    case 4:
    case 5:
        return ticks == s->count;
    default:
        return ticks >= s->count;
    }
}

/** Retain the current count until the complete latched value is read. */
static void pit_latch(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];

    if (!s->count_latched) {
        s->latched_count = pit_count(p, channel);
        s->count_latched = s->rw_mode;
    }
}

/** Retain the status byte independently of a latched count. */
static void pit_status(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];

    if (!s->status_latched) {
        s->status =
            (uint8_t)((pit_out(p, channel) << 7) | (s->rw_mode << 4) | (s->mode << 1) | s->bcd);
        s->status_latched = 1;
    }
}

/** Reload a counter and arm IRQ0 unless the mode or HPET disables it. */
static void pit_load(struct kvm_vm *vm, uint32_t channel, uint32_t count, int hpet_start)
{
    struct guest_pit *p = pit_of(vm);
    struct kvm_pit_channel_state *s = &p->state.channels[channel];
    uint64_t interval;

    s->count = count ? count : 65536u;
    if (channel) {
        s->count_load_time = (int64_t)kvm_host_ns();
        return;
    }
    if (hpet_start ||
        (s->mode != 0 && s->mode != 1 && s->mode != 2 && s->mode != 3 && s->mode != 4)) {
        p->active = 0;
        return;
    }
    if (p->state.flags & KVM_PIT_FLAGS_HPET_LEGACY) {
        return;
    }
    interval = (uint64_t)s->count * PIT_NS / PIT_HZ;
    p->periodic = s->mode == 2 || s->mode == 3;
    p->period = p->periodic && interval < PIT_MIN_PERIOD_NS ? PIT_MIN_PERIOD_NS : interval;
    p->expires = kvm_host_ns() + interval;
    p->pending = 0;
    p->inflight = 0;
    p->active = 1;
    pit_wake(vm);
}

/** Whether `p` may have work at `now`, read without the lock: an armed
 * expiry that passed, or a retained tick the guest acknowledged. */
static int pit_due(const struct guest_pit *p, uint64_t now)
{
    const volatile struct guest_pit *q = p;

    return (q->active && now >= q->expires) || (q->reinject && q->pending && !q->inflight);
}

/** A new tick or an acknowledged backlog may produce one IRQ0 pulse, at
 * KVM_REQ_TIMER, which the vCPU's deadline (`kvm_pit_next`) or a change of
 * the PIT (`pit_wake`) raises where Linux's PIT has an hrtimer
 * (arch/x86/kvm/i8254.c:268, 361). The clock is read only while a timer is
 * armed. */
void kvm_pit_check(struct kvm_vm *vm)
{
    struct guest_pit *p = pit_of(vm);
    uint64_t now, ticks = 0;
    uint32_t flags;

    if (!p) {
        return;
    }
    now = ((const volatile struct guest_pit *)p)->active ? kvm_host_ns() : 0;
    if (!pit_due(p, now)) {
        return;
    }
    flags = kvm_chip_lock(vm);
    if (p->active && now >= p->expires) {
        ticks = p->periodic ? (now - p->expires) / p->period + 1u : 1u;
        if (p->periodic) {
            p->expires += ticks * p->period;
        } else {
            p->active = 0;
        }
        if (p->reinject) {
            p->pending =
                ticks > 0xFFFFFFFFu - p->pending ? 0xFFFFFFFFu : p->pending + (uint32_t)ticks;
        }
    }
    if (p->reinject ? p->pending && !p->inflight : ticks != 0) {
        p->inflight = p->reinject;
        kvm_irq_set(vm, 0, KVM_IRQ_SRC_PIT, 1, 0);
        kvm_irq_set(vm, 0, KVM_IRQ_SRC_PIT, 0, 0);
        kvm_lapic_pit_nmi(vm);
    }
    kvm_chip_unlock(vm, flags);
}

/** The next timer deadline, or immediate work after an EOI of a backlog. */
uint64_t kvm_pit_next(struct kvm_vm *vm)
{
    struct guest_pit *p = pit_of(vm);
    uint64_t next;
    uint32_t flags;

    if (!p) {
        return 0;
    }
    flags = kvm_chip_lock(vm);
    if (p->reinject && p->pending && !p->inflight) {
        next = kvm_host_ns();
    } else {
        next = p->active ? p->expires : 0;
    }
    kvm_chip_unlock(vm, flags);
    return next;
}

/** Retire the delivered tick and wake vCPUs for any retained ticks. */
void kvm_pit_ack(struct kvm_vm *vm)
{
    struct guest_pit *p = pit_of(vm);

    if (p && p->reinject) {
        p->inflight = 0;
        if (p->pending) {
            p->pending--;
        }
        if (p->pending) {
            pit_wake(vm);
        }
    }
}

/** Reset the reinjection queue when IRQ0 is unmasked. */
void kvm_pit_unmask(struct kvm_vm *vm)
{
    struct guest_pit *p = pit_of(vm);

    if (p && p->reinject) {
        p->pending = 0;
        p->inflight = 0;
    }
}

/** Free the PIT; no host worker or callback outlives its VM. */
void kvm_pit_free(struct kvm_vm *vm)
{
    kfree(vm->pit);
    vm->pit = NULL;
}

/** Create a reset PIT and, when requested, its silent speaker port. */
static int32_t pit_create(struct kvm_vm *vm, uint32_t flags)
{
    struct guest_pit *p;
    uint32_t i, lock;

    if (vm->pit) {
        return -EEXIST;
    }
    if (vm->irqchip != KVM_IRQCHIP_KERNEL) {
        return -ENOENT;
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
        return -ENOMEM;
    }
    p->speaker = (flags & KVM_PIT_SPEAKER_DUMMY) != 0;
    p->reinject = 1;
    lock = kvm_chip_lock(vm);
    vm->pit = p;
    for (i = 0; i < 3u; i++) {
        p->state.channels[i].mode = 0xFF;
        p->state.channels[i].gate = i != 2;
        pit_load(vm, i, 0, 0);
    }
    kvm_chip_unlock(vm, lock);
    return 0;
}

/** PIT creation, save/restore and reinjection, in Linux's ioctl order,
 * under the big lock; each copies the state under the controllers' lock
 * and the user's memory outside it. */
int32_t kvm_pit_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    struct guest_pit *p = pit_of(vm);
    struct kvm_pit_config config;
    struct kvm_pit_state2 state;
    struct kvm_reinject_control control;
    uint32_t i, old_flags, flags;
    int hpet_start;

    switch (request) {
    case KVM_CREATE_PIT:
        return pit_create(vm, KVM_PIT_SPEAKER_DUMMY);
    case KVM_CREATE_PIT2:
        if (copy_from_user(&config, (const void __user *)arg, sizeof(config))) {
            return -EFAULT;
        }
        return pit_create(vm, config.flags);
    case KVM_GET_PIT:
        if (copy_from_user(&state.channels, (const void __user *)arg, sizeof(state.channels))) {
            return -EFAULT;
        }
        if (!p) {
            return -ENXIO;
        }
        flags = kvm_chip_lock(vm);
        k_memcpy(state.channels, p->state.channels, sizeof(state.channels));
        kvm_chip_unlock(vm, flags);
        return copy_to_user((void __user *)arg, state.channels, sizeof(state.channels)) ? -EFAULT
                                                                                        : 0;
    case KVM_GET_PIT2:
        if (!p) {
            return -ENXIO;
        }
        flags = kvm_chip_lock(vm);
        state = p->state;
        kvm_chip_unlock(vm, flags);
        k_memset(state.reserved, 0, sizeof(state.reserved));
        return copy_to_user((void __user *)arg, &state, sizeof(state)) ? -EFAULT : 0;
    case KVM_SET_PIT:
    case KVM_SET_PIT2:
        k_memset(&state, 0, sizeof(state));
        if (copy_from_user(&state, (const void __user *)arg,
                           request == KVM_SET_PIT ? sizeof(state.channels) : sizeof(state))) {
            return -EFAULT;
        }
        if (!p) {
            return -ENXIO;
        }
        flags = kvm_chip_lock(vm);
        old_flags = p->state.flags;
        if (request == KVM_SET_PIT) {
            state.flags = old_flags;
        }
        hpet_start =
            !(old_flags & KVM_PIT_FLAGS_HPET_LEGACY) && (state.flags & KVM_PIT_FLAGS_HPET_LEGACY);
        p->state = state;
        for (i = 0; i < 3u; i++) {
            pit_load(vm, i, state.channels[i].count, hpet_start && i == 0);
        }
        kvm_chip_unlock(vm, flags);
        return 0;
    case KVM_REINJECT_CONTROL:
        if (copy_from_user(&control, (const void __user *)arg, sizeof(control))) {
            return -EFAULT;
        }
        if (!p) {
            return -ENXIO;
        }
        flags = kvm_chip_lock(vm);
        if (!p->reinject && control.pit_reinject) {
            p->pending = 0;
            p->inflight = 0;
        }
        p->reinject = control.pit_reinject != 0;
        kvm_chip_unlock(vm, flags);
        return 0;
    default:
        return -ENOTTY;
    }
}

/** Program a counter's format or latch one or more counts and statuses. */
static void pit_control(struct guest_pit *p, uint8_t value)
{
    uint32_t channel = value >> 6, access = (value >> 4) & 3u;
    struct kvm_pit_channel_state *s;

    if (channel == 3u) {
        for (channel = 0; channel < 3u; channel++) {
            if (value & (2u << channel)) {
                if (!(value & 0x20u)) {
                    pit_latch(p, channel);
                }
                if (!(value & 0x10u)) {
                    pit_status(p, channel);
                }
            }
        }
        return;
    }
    if (!access) {
        pit_latch(p, channel);
        return;
    }
    s = &p->state.channels[channel];
    s->rw_mode = s->read_state = s->write_state = (uint8_t)access;
    s->mode = (value >> 1) & 7u;
    if (s->mode > 5u) {
        s->mode -= 4u;
    }
    s->bcd = value & 1u;
}

/** Consume one count byte, loading only after the last byte arrives. */
static void pit_write_count(struct kvm_vm *vm, uint32_t channel, uint8_t value)
{
    struct kvm_pit_channel_state *s = &pit_of(vm)->state.channels[channel];

    switch (s->write_state) {
    case PIT_MSB:
        pit_load(vm, channel, (uint32_t)value << 8, 0);
        break;
    case PIT_WORD_LOW:
        s->write_latch = value;
        s->write_state = PIT_WORD_HIGH;
        break;
    case PIT_WORD_HIGH:
        pit_load(vm, channel, s->write_latch | ((uint32_t)value << 8), 0);
        s->write_state = PIT_WORD_LOW;
        break;
    default:
        pit_load(vm, channel, value, 0);
        break;
    }
}

/** Read status first, then a latched count, then the running counter. */
static uint32_t pit_read_count(struct guest_pit *p, uint32_t channel)
{
    struct kvm_pit_channel_state *s = &p->state.channels[channel];
    uint32_t count;
    uint8_t state;

    if (s->status_latched) {
        s->status_latched = 0;
        return s->status;
    }
    if (s->count_latched) {
        state = s->count_latched;
        count = s->latched_count;
        s->count_latched = state == PIT_WORD_LOW ? PIT_MSB : 0;
        return state == PIT_MSB ? count >> 8 : count & 0xFFu;
    }
    count = pit_count(p, channel);
    state = s->read_state;
    if (state == PIT_WORD_LOW || state == PIT_WORD_HIGH) {
        s->read_state = state == PIT_WORD_LOW ? PIT_WORD_HIGH : PIT_WORD_LOW;
    }
    return state == PIT_MSB || state == PIT_WORD_HIGH ? count >> 8 : count & 0xFFu;
}

/** Read or write the PIT and optional silent speaker port. */
int kvm_pit_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write)
{
    struct guest_pit *p = pit_of(vm);
    struct kvm_pit_channel_state *s;
    uint32_t channel;

    (void)size;
    if (!p) {
        return 0;
    }
    if (port == 0x61u && p->speaker) {
        s = &p->state.channels[2];
        if (write) {
            p->state.flags = (p->state.flags & ~KVM_PIT_FLAGS_SPEAKER_DATA_ON) |
                             (*value & KVM_PIT_FLAGS_SPEAKER_DATA_ON);
            if (s->mode != 0 && s->mode != 4 && s->gate < (*value & 1u)) {
                s->count_load_time = (int64_t)kvm_host_ns();
            }
            s->gate = (uint8_t)*value & 1u;
        } else {
            *value = (p->state.flags & KVM_PIT_FLAGS_SPEAKER_DATA_ON) | s->gate |
                     (pit_out(p, 2) << 5) | (uint32_t)((kvm_host_ns() >> 14) & 1u) << 4;
        }
        return 1;
    }
    if (port < 0x40u || port > 0x43u) {
        return 0;
    }
    channel = port & 3u;
    if (write) {
        if (channel == 3u) {
            pit_control(p, (uint8_t)*value);
        } else {
            pit_write_count(vm, channel, (uint8_t)*value);
        }
    } else {
        *value = channel == 3u ? 0 : pit_read_count(p, channel);
    }
    return 1;
}
