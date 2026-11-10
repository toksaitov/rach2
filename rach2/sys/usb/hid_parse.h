/*
 * hid_parse.h: a HID report descriptor, parsed as Linux's hid-core.c parses
 * one, into the reports, fields and usages the input mapping reads.
 */
#ifndef _USB_HID_PARSE_H_
#define _USB_HID_PARSE_H_

#include "../lib/kint.h"

/* The three kinds of report, as Linux numbers them. */
#define HID_INPUT_REPORT   0
#define HID_OUTPUT_REPORT  1
#define HID_FEATURE_REPORT 2

/* The bits of an Input, Output or Feature item's data, a field's `flags`. */
#define HID_MAIN_ITEM_CONSTANT      0x001
#define HID_MAIN_ITEM_VARIABLE      0x002 /* else an array of usage indexes */
#define HID_MAIN_ITEM_RELATIVE      0x004
#define HID_MAIN_ITEM_WRAP          0x008
#define HID_MAIN_ITEM_NONLINEAR     0x010
#define HID_MAIN_ITEM_NO_PREFERRED  0x020
#define HID_MAIN_ITEM_NULL_STATE    0x040
#define HID_MAIN_ITEM_VOLATILE      0x080
#define HID_MAIN_ITEM_BUFFERED_BYTE 0x100

/* A collection's type, the data of its Collection item. */
#define HID_COLLECTION_PHYSICAL    0
#define HID_COLLECTION_APPLICATION 1
#define HID_COLLECTION_LOGICAL     2
#define HID_COLLECTION_NAMED_ARRAY 4

/* Linux's bounds: the largest descriptor usbhid reads, the usages a field
 * may have, the report IDs, the fields of one report, the depth of Push,
 * and the largest report with its ID byte. */
#define HID_MAX_DESCRIPTOR_SIZE 4096
#define HID_MAX_USAGES          12288
#define HID_MAX_IDS             256
#define HID_MAX_FIELDS          256
#define HID_GLOBAL_STACK_SIZE   4
#define HID_MAX_BUFFER_SIZE     16384

/* This kernel's bounds, past which a descriptor Linux would take is refused
 * with -E2BIG: the reports of all three kinds, and how deep collections
 * nest. No device seen has a tenth of either. */
#define HID_MAX_REPORTS     64
#define HID_MAX_COLLECTIONS 32

/* A field that belongs to no input device, `hid_field.input`. */
#define HID_NO_INPUT 0xFF

/*
 * One usage of a field, Linux's `struct hid_usage`, with the value of the
 * field's element of the same index: a field has at least as many usages as
 * elements, so element n's values are kept in usage n.
 */
struct hid_usage {
    uint32_t hid;        /* the usage page in the high half, the usage in the low */
    uint16_t collection; /* the collection the usage was declared in */
    uint16_t code;       /* the event code hid-input mapped it to */
    uint8_t type;        /* the event type, EV_*, or 0 for none */
    uint8_t size;        /* the size of its Usage item, during the parse */
    int8_t hat_dir;      /* a D-pad direction, 1 to 8, or 0 */
    uint8_t hat;         /* a hat switch, whose value is a direction */
    int16_t multiplier;  /* a wheel's Resolution Multiplier, 1 without one */
    int32_t value;       /* element's value in the last report */
    int32_t new_value;   /* element's value in the report being read */
    int32_t wheel;       /* a hi-res wheel's 120ths not yet a detent */
};

/* One field, the elements of one Input, Output or Feature item that has a
 * usage, Linux's `struct hid_field`. */
struct hid_field {
    uint16_t report;   /* its report, an index in `hid_desc.reports` */
    uint8_t input;     /* its input device, or HID_NO_INPUT */
    uint8_t ignored;   /* the report being read had ErrorRollOver here */
    uint32_t flags;    /* HID_MAIN_ITEM_* */
    uint32_t offset;   /* its first bit in the report, after the ID byte */
    uint32_t size;     /* the bits of one element */
    uint32_t count;    /* its elements */
    uint32_t maxusage; /* its usages, at least `count` */
    uint32_t usage;    /* its first usage, an index in `hid_desc.usages` */
    int32_t logical_minimum;
    int32_t logical_maximum;
    int32_t physical_minimum;
    int32_t physical_maximum;
    int32_t unit_exponent;
    uint32_t unit;
    uint32_t application; /* the usage of the application collection it is in */
    uint32_t physical;    /* of the nearest physical collection, or 0 */
    uint32_t logical;     /* of the nearest logical collection, or 0 */
    uint16_t dpad;        /* the ABS code its D-pad's usages share, or 0 */
};

/* One report, Linux's `struct hid_report`. */
struct hid_report {
    uint8_t type;         /* HID_*_REPORT */
    uint8_t id;           /* its ID, 0 on a device that numbers none */
    uint32_t size;        /* its bits, without the ID byte */
    uint32_t application; /* the application its first field is in */
};

/* One collection, Linux's `struct hid_collection`. */
struct hid_collection {
    uint32_t usage;
    uint8_t type;   /* HID_COLLECTION_* */
    uint16_t level; /* how deep it is, 0 for a top-level collection */
    int16_t parent; /* the collection it is in, or -1 */
};

/*
 * A parsed descriptor. The caller gives the arrays: `hid_parse` with
 * `fields` NULL only counts, leaving in `max_fields`, `max_usages` and
 * `max_collections` the sizes the arrays must have, and then fills arrays
 * of those sizes. The reports are few and kept here.
 */
struct hid_desc {
    struct hid_field *fields;
    struct hid_usage *usages;
    struct hid_collection *collections;
    uint32_t max_fields;
    uint32_t max_usages;
    uint32_t max_collections;
    uint32_t nfields;
    uint32_t nusages;
    uint32_t ncollections;
    uint32_t maxapplication; /* the application collections */
    uint32_t nreports;
    uint8_t numbered[3]; /* the reports of a kind carry an ID byte */
    struct hid_report reports[HID_MAX_REPORTS];
};

/**
 * Parse the report descriptor `rdesc` of `length` bytes into `desc`, as
 * Linux's `hid_open_report` does.
 *
 * `desc` is zeroed first but for its three array pointers and their sizes.
 * With `desc->fields` NULL nothing is stored but the counts, and the sizes
 * the arrays need are left in `max_fields`, `max_usages` and
 * `max_collections`; the caller gives arrays of those sizes and calls
 * again, which fills them. Returns 0; -EINVAL for a descriptor Linux
 * refuses: an item cut short, a long item, an unknown global item, a
 * Push or Pop past the stack, a Report Size over 256, a Report Count or a
 * field's usages over HID_MAX_USAGES, a Report ID of 0 or over 255, a
 * logical maximum below the minimum, a report longer than
 * HID_MAX_BUFFER_SIZE, an End Collection or a delimiter with nothing open,
 * nested delimiters, and a descriptor that is empty or ends inside a
 * collection or a delimiter; -E2BIG for one past this kernel's bounds,
 * more than HID_MAX_REPORTS reports, collections nested deeper than
 * HID_MAX_COLLECTIONS, or arrays smaller than the counting asked for.
 * Fields past HID_MAX_FIELDS in one report are dropped, as Linux drops
 * them. Any context; it allocates nothing.
 */
int hid_parse(struct hid_desc *desc, const uint8_t *rdesc, int length);

/**
 * The report of kind `type` and ID `id` in `desc`, or NULL. On a device
 * whose reports of that kind are not numbered, the one report has ID 0.
 */
struct hid_report *hid_find_report(struct hid_desc *desc, int type, int id);

/** The bytes of report `r` on the wire, its ID byte among them, Linux's
 * `hid_report_len`. */
uint32_t hid_report_len(const struct hid_report *r);

/** Read `n` bits, 0 to 32, at bit `offset` of the little-endian report
 * `report`, as Linux's `hid_field_extract`. */
uint32_t hid_field_extract(const uint8_t *report, uint32_t offset, uint32_t n);

/** Write the low `n` bits of `value` at bit `offset` of `report`, as
 * Linux's `implement`; the other bits are kept. */
void hid_field_implement(uint8_t *report, uint32_t offset, uint32_t n, uint32_t value);

/** The `n`-bit two's complement `value` as a signed 32-bit number, Linux's
 * `hid_snto32`. */
int32_t hid_snto32(uint32_t value, uint32_t n);

#endif
