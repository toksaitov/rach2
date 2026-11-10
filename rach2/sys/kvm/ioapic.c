/* The guest's 24-pin IOAPIC, with Linux's routing and EOI state.
 * Port, ioctl and deferred irqfd operations hold the big lock. */

#include "kvm.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

#define IOA_DELIVERY (1ull << 12)
#define IOA_REMOTE   (1ull << 14)
#define IOA_LEVEL    (1ull << 15)
#define IOA_MASKED   (1ull << 16)
#define IOA_READONLY (IOA_DELIVERY | IOA_REMOTE)
#define IOA_RTC_PIN  8u

struct guest_ioapic {
    struct kvm_ioapic_state state;
    uint32_t sources[KVM_IOAPIC_NUM_PINS]; /* independent KVM_IRQ_SRC_* */
    uint32_t delivered;                    /* edge requests delivered since the last line update */
    uint64_t rtc_pending;                  /* vCPU indices awaiting the RTC vector's EOI */
    uint8_t rtc_vector[KVM_MAX_VCPUS];     /* vector at each tracked delivery */
    uint32_t counts[KVM_IOAPIC_NUM_PINS][KVM_PIN_STEPS]; /* by enum kvm_pin_step */
};

/** The IOAPIC of `vm`, or NULL before KVM_CREATE_IRQCHIP. */
static struct guest_ioapic *ioapic_of(struct kvm_vm *vm)
{
    return (struct guest_ioapic *)vm->ioapic;
}

/** Drop RTC acknowledgements whose vectors have left both IRR and ISR. */
static void ioapic_rtc_sync(struct kvm_vm *vm)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        if ((p->rtc_pending & (1ull << i)) &&
            !kvm_lapic_pending_eoi(vm->vcpus[i], p->rtc_vector[i])) {
            p->rtc_pending &= ~(1ull << i);
        }
    }
}

/** Deliver one unmasked pin and latch remote IRR for a level interrupt. */
static int ioapic_deliver(struct kvm_vm *vm, uint32_t pin, int status)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint64_t entry = p->state.redirtbl[pin], destinations = 0;
    struct kvm_lapic_irq irq;
    uint32_t i;
    int result;

    if ((entry & IOA_MASKED) || ((entry & IOA_LEVEL) && (entry & IOA_REMOTE))) {
        p->counts[pin][KVM_PIN_HELD]++;
        return -1;
    }
    irq.vector = (uint32_t)entry & 0xFFu;
    irq.delivery_mode = (uint32_t)entry & 0x700u;
    irq.dest_mode = ((uint32_t)entry >> 11) & 1u;
    irq.trig_mode = (entry & IOA_LEVEL) != 0;
    irq.level = 1;
    irq.shorthand = 0;
    irq.dest_id = (uint32_t)(entry >> 56);
    irq.redir_hint = 0;
    if (!(entry & IOA_LEVEL)) {
        p->delivered |= 1u << pin;
    }
    result = kvm_lapic_deliver_tracked(vm, NULL, &irq,
                                       pin == IOA_RTC_PIN && status ? &destinations : NULL);
    if (pin == IOA_RTC_PIN && status) {
        p->rtc_pending = destinations;
        for (i = 0; i < vm->nr_vcpus; i++) {
            if (destinations & (1ull << i)) {
                p->rtc_vector[i] = (uint8_t)irq.vector;
            }
        }
    }
    p->counts[pin][result > 0 ? KVM_PIN_DELIVER : KVM_PIN_MISS]++;
    if (result > 0 && (entry & IOA_LEVEL)) {
        p->state.redirtbl[pin] |= IOA_REMOTE;
    }
    return result;
}

/** Apply the combined pin level, retaining masked edges as Linux does. */
static int ioapic_level(struct kvm_vm *vm, uint32_t pin, int level, int status)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t bit = 1u << pin, old = p->state.irr;

    if (!level) {
        p->state.irr &= ~bit;
        return 1;
    }
    p->counts[pin][KVM_PIN_RAISE]++;
    if (pin == IOA_RTC_PIN && status) {
        ioapic_rtc_sync(vm);
        if (p->rtc_pending) {
            p->counts[pin][KVM_PIN_COALESCE]++;
            return 0;
        }
    }
    p->state.irr |= bit;
    if (!(p->state.redirtbl[pin] & IOA_LEVEL)) {
        p->delivered &= ~bit;
        if (old & bit) {
            p->counts[pin][KVM_PIN_COALESCE]++;
            return 0;
        }
    }
    return ioapic_deliver(vm, pin, status);
}

/** Copy the counts of `pin` into `counts`, KVM_PIN_STEPS of them by enum
 * kvm_pin_step; 0, or -1 with no IOAPIC in the kernel or no such pin. */
int kvm_ioapic_counts(struct kvm_vm *vm, uint32_t pin, uint32_t *counts)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t i;

    if (!p || pin >= KVM_IOAPIC_NUM_PINS) {
        return -1;
    }
    for (i = 0; i < KVM_PIN_STEPS; i++) {
        counts[i] = p->counts[pin][i];
    }
    return 0;
}

/** Allocate a reset IOAPIC, all entries masked, at 0xFEC00000. */
int kvm_ioapic_create(struct kvm_vm *vm)
{
    struct guest_ioapic *p = kzalloc(sizeof(*p));
    uint32_t pin;

    if (!p) {
        return -ENOMEM;
    }
    p->state.base_address = 0xFEC00000ull;
    for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++) {
        p->state.redirtbl[pin] = IOA_MASKED;
    }
    vm->ioapic = p;
    return 0;
}

/** Free the IOAPIC after its interrupt producers are detached. */
void kvm_ioapic_free(struct kvm_vm *vm)
{
    kfree(vm->ioapic);
    vm->ioapic = NULL;
}

/** Change one input source and apply the OR of all sources of the pin. */
int kvm_ioapic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level, int status)
{
    struct guest_ioapic *p = ioapic_of(vm);

    if (!p || pin >= KVM_IOAPIC_NUM_PINS) {
        return -1;
    }
    if (level) {
        p->sources[pin] |= source;
    } else {
        p->sources[pin] &= ~source;
    }
    return ioapic_level(vm, pin, p->sources[pin] != 0, status);
}

/** Broadcast an EOI to matching pins and then redeliver asserted levels. */
void kvm_ioapic_eoi(struct kvm_vcpu *v, uint32_t vector, int level, int directed)
{
    struct kvm_vm *vm = v->vm;
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t pin;
    uint64_t *entry;

    if (!p) {
        return;
    }
    if (v->index < KVM_MAX_VCPUS && p->rtc_vector[v->index] == vector) {
        p->rtc_pending &= ~(1ull << v->index);
    }
    for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++) {
        entry = &p->state.redirtbl[pin];
        if ((*entry & 0xFFu) != vector) {
            continue;
        }
        kvm_irq_ack(vm, KVM_IRQCHIP_IOAPIC, pin);
        if (!level || directed) {
            continue;
        }
        *entry &= ~IOA_REMOTE;
        if (p->state.irr & (1u << pin)) {
            ioapic_deliver(vm, pin, 0);
        }
    }
}

/** Reconstruct the RTC's outstanding EOI after an APIC state restore. */
void kvm_ioapic_restore(struct kvm_vcpu *v)
{
    struct guest_ioapic *p = ioapic_of(v->vm);
    uint64_t entry, bit;
    uint32_t vector;

    if (!p || v->index >= KVM_MAX_VCPUS) {
        return;
    }
    entry = p->state.redirtbl[IOA_RTC_PIN];
    if (!kvm_lapic_match_dest(v, (uint32_t)(entry >> 56), (entry & (1u << 11)) != 0)) {
        return;
    }
    bit = 1ull << v->index;
    vector = (uint32_t)entry & 0xFFu;
    if (kvm_lapic_pending_eoi(v, vector)) {
        p->rtc_pending |= bit;
        p->rtc_vector[v->index] = (uint8_t)vector;
    } else {
        p->rtc_pending &= ~bit;
    }
}

/** Copy Linux's IOAPIC record; a restore reinjects its pending inputs. */
void kvm_ioapic_state(struct kvm_vm *vm, struct kvm_ioapic_state *state, int set)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t pin;

    if (!set) {
        *state = p->state;
        state->irr &= ~p->delivered;
        return;
    }
    p->state = *state;
    p->delivered = 0;
    p->rtc_pending = 0;
    for (pin = 0; pin < KVM_IOAPIC_NUM_PINS; pin++) {
        if (state->irr & (1u << pin)) {
            p->state.irr &= ~(1u << pin);
            ioapic_level(vm, pin, 1, 1);
        }
    }
    for (pin = 0; pin < vm->nr_vcpus; pin++) {
        kvm_ioapic_restore(vm->vcpus[pin]);
    }
}

/** Read the indirect register selected by IOREGSEL. */
static uint32_t ioapic_read(struct guest_ioapic *p)
{
    uint32_t reg = p->state.ioregsel, pin = (reg - 0x10u) / 2u;
    uint64_t entry;

    if (reg == 0 || reg == 2) {
        return (p->state.id & 15u) << 24;
    }
    if (reg == 1) {
        return ((KVM_IOAPIC_NUM_PINS - 1u) << 16) | 0x11u;
    }
    entry = pin < KVM_IOAPIC_NUM_PINS ? p->state.redirtbl[pin] : ~0ull;
    return (uint32_t)(reg & 1u ? entry >> 32 : entry);
}

/** Write an indirect register, retaining the read-only delivery bits. */
static void ioapic_write(struct kvm_vm *vm, uint32_t value)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t reg = p->state.ioregsel, pin = (reg - 0x10u) / 2u;
    uint64_t old, entry;

    if (reg == 0) {
        p->state.id = (value >> 24) & 15u;
        return;
    }
    if (reg < 0x10u || pin >= KVM_IOAPIC_NUM_PINS) {
        return;
    }
    old = p->state.redirtbl[pin];
    entry =
        reg & 1u ? (old & 0xFFFFFFFFull) | ((uint64_t)value << 32) : (old & ~0xFFFFFFFFull) | value;
    entry = (entry & ~IOA_READONLY) | (old & IOA_READONLY);
    if (!(entry & IOA_LEVEL)) {
        entry &= ~IOA_REMOTE;
    }
    p->state.redirtbl[pin] = entry;
    if ((old ^ entry) & IOA_MASKED) {
        kvm_irq_mask(vm, KVM_IRQCHIP_IOAPIC, pin, (entry & IOA_MASKED) != 0);
    }
    if ((entry & IOA_LEVEL) && !(entry & (IOA_MASKED | IOA_REMOTE)) &&
        (p->state.irr & (1u << pin))) {
        if (!kvm_irq_resample(vm, KVM_IRQCHIP_IOAPIC, pin)) {
            ioapic_deliver(vm, pin, 0);
        }
    }
}

/** Handle the IOAPIC's MMIO page with the register aliases Linux exposes. */
int kvm_ioapic_io(struct kvm_vm *vm, uint64_t addr, uint32_t size, uint64_t *value, int write)
{
    struct guest_ioapic *p = ioapic_of(vm);
    uint32_t offset;

    if (!p || addr < p->state.base_address || addr - p->state.base_address >= 0x1000u) {
        return 0;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return 1;
    }
    offset = (uint32_t)addr & 0xFFu;
    if (write) {
        if (offset == 0) {
            p->state.ioregsel = (uint32_t)*value & 0xFFu;
        } else if (offset == 0x10u) {
            ioapic_write(vm, (uint32_t)*value);
        }
    } else {
        *value = offset == 0 ? p->state.ioregsel : (offset == 0x10u ? ioapic_read(p) : 0);
    }
    return 1;
}
