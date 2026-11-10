/* The guest's cascaded 8259 PICs, their ports and Linux's saved state.
 * All state changes run under the big lock; irqfd callbacks defer to it. */

#include "kvm.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

struct guest_pic {
    struct kvm_pic_state chip[2];
    uint32_t sources[16]; /* ORed KVM_IRQ_SRC_* at each input */
    int output;           /* the master's INT pin */
};

/** The PIC pair of a VM, or NULL with a split or absent controller. */
static struct guest_pic *pic_of(struct kvm_vm *vm)
{
    return (struct guest_pic *)vm->pic;
}

/** The priority of the first bit in `mask`, or 8 with no bit set. */
static int pic_priority(const struct kvm_pic_state *s, uint32_t mask)
{
    int n;

    for (n = 0; n < 8; n++) {
        if (mask & (1u << ((n + s->priority_add) & 7))) {
            return n;
        }
    }
    return 8;
}

/** The pending pin that outranks the pins in service, or -1. */
static int pic_pin(struct guest_pic *p, uint32_t chip)
{
    struct kvm_pic_state *s = &p->chip[chip];
    uint32_t service = s->isr;
    int priority = pic_priority(s, s->irr & ~s->imr);

    if (chip == 0 && s->special_fully_nested_mode) {
        service &= ~4u;
    }
    return priority < pic_priority(s, service) ? (priority + s->priority_add) & 7 : -1;
}

/** Apply one input level, retaining an edge until it is acknowledged. */
static int pic_level(struct kvm_pic_state *s, uint32_t pin, int level)
{
    uint8_t bit = (uint8_t)(1u << pin);
    int result = 1;

    if (level) {
        if ((s->elcr & bit) || !(s->last_irr & bit)) {
            result = !(s->irr & bit);
            s->irr |= bit;
        }
        s->last_irr |= bit;
    } else {
        s->last_irr &= (uint8_t)~bit;
        if (s->elcr & bit) {
            s->irr &= (uint8_t)~bit;
        }
    }
    return s->imr & bit ? -1 : result;
}

/** Recompute the cascade and INT, waking a vCPU that accepts the PIC. */
static void pic_update(struct kvm_vm *vm)
{
    struct guest_pic *p = pic_of(vm);

    if (pic_pin(p, 1) >= 0) {
        pic_level(&p->chip[0], 2, 1);
        pic_level(&p->chip[0], 2, 0);
    }
    p->output = pic_pin(p, 0) >= 0;
    if (p->output) {
        kvm_lapic_pic_wake(vm);
    }
}

/** End service of one pin before notifying its PIT or irqfd producer. */
static void pic_eoi(struct kvm_vm *vm, uint32_t chip, uint32_t pin)
{
    pic_of(vm)->chip[chip].isr &= (uint8_t)~(1u << pin);
    kvm_irq_ack(vm, chip, pin);
}

/** Acknowledge one pin, applying automatic EOI and rotation. */
static void pic_take(struct kvm_vm *vm, uint32_t chip, uint32_t pin)
{
    struct kvm_pic_state *s = &pic_of(vm)->chip[chip];
    uint8_t bit = (uint8_t)(1u << pin);

    s->isr |= bit;
    if (!(s->elcr & bit)) {
        s->irr &= (uint8_t)~bit;
    }
    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi) {
            s->priority_add = (uint8_t)((pin + 1u) & 7u);
        }
        pic_eoi(vm, chip, pin);
    }
}

/** Allocate the pair in Linux's power-on state. */
int kvm_pic_create(struct kvm_vm *vm)
{
    struct guest_pic *p = kzalloc(sizeof(*p));

    if (!p) {
        return -ENOMEM;
    }
    p->chip[0].elcr_mask = 0xF8;
    p->chip[1].elcr_mask = 0xDE;
    vm->pic = p;
    return 0;
}

/** Release the pair after its producers have been detached. */
void kvm_pic_free(struct kvm_vm *vm)
{
    kfree(vm->pic);
    vm->pic = NULL;
}

/** Apply a source to a global PIC pin, 0 to 15, under the big lock. */
int kvm_pic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level)
{
    struct guest_pic *p = pic_of(vm);
    int result;

    if (!p || pin >= 16u) {
        return -1;
    }
    if (level) {
        p->sources[pin] |= source;
    } else {
        p->sources[pin] &= ~source;
    }
    result = pic_level(&p->chip[pin / 8u], pin & 7u, p->sources[pin] != 0);
    pic_update(vm);
    return result;
}

/** Inspect the master's INT and the cascaded vector without an INTA. */
int kvm_pic_pending(struct kvm_vm *vm)
{
    struct guest_pic *p = pic_of(vm);
    int pin, chip = 0;

    if (!p || !p->output) {
        return -1;
    }
    pin = pic_pin(p, 0);
    if (pin == 2) {
        chip = 1;
        pin = pic_pin(p, 1);
    }
    return p->chip[chip].irq_base + (pin < 0 ? 7 : pin);
}

/** Perform INTA on the master and, for IRQ2, on the slave. */
int kvm_pic_ack(struct kvm_vm *vm)
{
    struct guest_pic *p = pic_of(vm);
    int pin, chip = 0, vector;

    if (!p) {
        return -1;
    }
    p->output = 0;
    pin = pic_pin(p, 0);
    if (pin >= 0) {
        pic_take(vm, 0, (uint32_t)pin);
        if (pin == 2) {
            chip = 1;
            pin = pic_pin(p, 1);
            if (pin >= 0) {
                pic_take(vm, 1, (uint32_t)pin);
            }
        }
    }
    vector = p->chip[chip].irq_base + (pin < 0 ? 7 : pin);
    pic_update(vm);
    return vector;
}

/** Copy a PIC's ABI record, recomputing the cascade after a restore. */
void kvm_pic_state(struct kvm_vm *vm, uint32_t chip, struct kvm_pic_state *state, int set)
{
    struct kvm_pic_state *s = &pic_of(vm)->chip[chip];

    if (set) {
        *s = *state;
        pic_update(vm);
    } else {
        *state = *s;
    }
}

/** Start ICW initialization while preserving level inputs as Linux does. */
static void pic_init_command(struct kvm_vm *vm, uint32_t chip, uint8_t value)
{
    struct kvm_pic_state *s = &pic_of(vm)->chip[chip];
    uint8_t edges = s->irr & (uint8_t)~s->elcr;
    uint32_t pin;

    s->init4 = value & 1u;
    s->last_irr = 0;
    s->irr &= s->elcr;
    s->imr = 0;
    s->priority_add = 0;
    s->special_mask = 0;
    s->read_reg_select = 0;
    if (!s->init4) {
        s->special_fully_nested_mode = 0;
        s->auto_eoi = 0;
    }
    s->init_state = 1;
    for (pin = 0; pin < 8u; pin++) {
        if (edges & (1u << pin)) {
            pic_eoi(vm, chip, pin);
        }
    }
}

/** Apply OCW2's EOI and priority rotation. */
static void pic_command(struct kvm_vm *vm, uint32_t chip, uint8_t value)
{
    struct kvm_pic_state *s = &pic_of(vm)->chip[chip];
    uint32_t command = value >> 5;
    int priority, pin;

    if (command == 0 || command == 4) {
        s->rotate_on_auto_eoi = (uint8_t)(command >> 2);
        return;
    }
    if (command == 6) {
        s->priority_add = (uint8_t)((value + 1u) & 7u);
    } else if (command == 1 || command == 3 || command == 5 || command == 7) {
        priority = pic_priority(s, s->isr);
        pin = command & 2u ? value & 7 : (priority + s->priority_add) & 7;
        if (!(command & 2u) && priority == 8) {
            return;
        }
        if (command & 4u) {
            s->priority_add = (uint8_t)((pin + 1) & 7);
        }
        pic_eoi(vm, chip, (uint32_t)pin);
    }
    pic_update(vm);
}

/** Write one command or data byte. */
static void pic_write(struct kvm_vm *vm, uint32_t chip, uint32_t port, uint8_t value)
{
    struct kvm_pic_state *s = &pic_of(vm)->chip[chip];
    uint32_t pin;
    uint8_t changed;

    if (!(port & 1u)) {
        if (value & 0x10u) {
            pic_init_command(vm, chip, value);
        } else if (value & 8u) {
            if (value & 4u) {
                s->poll = 1;
            }
            if (value & 2u) {
                s->read_reg_select = value & 1u;
            }
            if (value & 0x40u) {
                s->special_mask = (value >> 5) & 1u;
            }
        } else {
            pic_command(vm, chip, value);
        }
        return;
    }
    switch (s->init_state) {
    case 0:
        changed = s->imr ^ value;
        s->imr = value;
        for (pin = 0; pin < 8u; pin++) {
            if (changed & (1u << pin)) {
                kvm_irq_mask(vm, chip, pin, (value & (1u << pin)) != 0);
            }
        }
        pic_update(vm);
        break;
    case 1:
        s->irq_base = value & 0xF8u;
        s->init_state = 2;
        break;
    case 2:
        s->init_state = s->init4 ? 3 : 0;
        break;
    case 3:
        s->special_fully_nested_mode = (value >> 4) & 1u;
        s->auto_eoi = (value >> 1) & 1u;
        s->init_state = 0;
        break;
    default:
        break;
    }
}

/** Read a command/data port, including OCW3's poll acknowledgement. */
static uint32_t pic_read(struct kvm_vm *vm, uint32_t chip, uint32_t port)
{
    struct guest_pic *p = pic_of(vm);
    struct kvm_pic_state *s = &p->chip[chip];
    int pin;

    if (!s->poll) {
        return port & 1u ? s->imr : (s->read_reg_select ? s->isr : s->irr);
    }
    s->poll = 0;
    pin = pic_pin(p, chip);
    if (pin >= 0) {
        if (chip) {
            p->chip[0].isr &= (uint8_t)~4u;
            p->chip[0].irr &= (uint8_t)~4u;
        }
        s->irr &= (uint8_t)~(1u << pin);
        pic_eoi(vm, chip, (uint32_t)pin);
        if (chip || pin != 2) {
            pic_update(vm);
        }
        return (uint32_t)pin | 0x80u;
    }
    pic_update(vm);
    return 7;
}

/** Dispatch a PIC or ELCR port; non-byte accesses are consumed as Linux's. */
int kvm_pic_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write)
{
    struct guest_pic *p = pic_of(vm);
    struct kvm_pic_state *s;
    uint32_t chip;

    if (!p || (port != 0x20u && port != 0x21u && port != 0xA0u && port != 0xA1u && port != 0x4D0u &&
               port != 0x4D1u)) {
        return 0;
    }
    if (size != 1) {
        *value = 0;
        return 1;
    }
    if (port >= 0x4D0u) {
        s = &p->chip[port & 1u];
        if (write) {
            s->elcr = (uint8_t)*value & s->elcr_mask;
        } else {
            *value = s->elcr;
        }
        return 1;
    }
    chip = port >> 7;
    if (write) {
        pic_write(vm, chip, port, (uint8_t)*value);
    } else {
        *value = pic_read(vm, chip, port);
    }
    return 1;
}
