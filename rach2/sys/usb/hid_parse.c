/*
 * hid_parse.c: the HID report descriptor parser, Linux's hid-core.c's.
 *
 * A report descriptor is a stream of short items. A main item (Input,
 * Output, Feature, Collection, End Collection) acts on the state the
 * global and local items before it built: the globals (usage page, logical
 * and physical range, report size, count and ID) stay until changed, and
 * the locals (usages and usage ranges) are spent by the next main item.
 * Each Input, Output or Feature item with a usage becomes a field of its
 * report, at the bit where the report's previous items ended.
 *
 * Linux keeps a local usage table of HID_MAX_USAGES entries while it
 * parses. Here a local usage is written straight into the caller's usage
 * array, past the usages fields have taken, and is kept when a field takes
 * it; so the parse runs twice, once counting and once filling arrays of
 * the counted sizes, and needs no table of its own.
 */

#include "hid_parse.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The item types and tags, HID 1.11 section 6.2.2. */
#define ITEM_MAIN     0
#define ITEM_GLOBAL   1
#define ITEM_LOCAL    2
#define ITEM_RESERVED 3
#define TAG_LONG      15

#define MAIN_INPUT           8
#define MAIN_OUTPUT          9
#define MAIN_COLLECTION      10
#define MAIN_FEATURE         11
#define MAIN_END_COLLECTION  12
#define GLOBAL_USAGE_PAGE    0
#define GLOBAL_LOGICAL_MIN   1
#define GLOBAL_LOGICAL_MAX   2
#define GLOBAL_PHYSICAL_MIN  3
#define GLOBAL_PHYSICAL_MAX  4
#define GLOBAL_UNIT_EXPONENT 5
#define GLOBAL_UNIT          6
#define GLOBAL_REPORT_SIZE   7
#define GLOBAL_REPORT_ID     8
#define GLOBAL_REPORT_COUNT  9
#define GLOBAL_PUSH          10
#define GLOBAL_POP           11
#define LOCAL_USAGE          0
#define LOCAL_USAGE_MINIMUM  1
#define LOCAL_USAGE_MAXIMUM  2
#define LOCAL_DELIMITER      10

/* One short item: its type, tag, data bytes (0, 1, 2 or 4) and data. */
struct item {
    unsigned type;
    unsigned tag;
    unsigned size;
    uint32_t data;
};

/* The global items' state, which Push and Pop save and restore. */
struct global {
    uint32_t usage_page;
    int32_t logical_minimum;
    int32_t logical_maximum;
    int32_t physical_minimum;
    int32_t physical_maximum;
    int32_t unit_exponent;
    uint32_t unit;
    uint32_t report_id;
    uint32_t report_size;
    uint32_t report_count;
};

/* The local items' state, cleared by each main item. The usages themselves
 * are `desc->usages[taken + i]` for i below `usage_index`. */
struct local {
    uint32_t usage_index;
    uint32_t usage_minimum;
    uint32_t delimiter_depth;
    uint32_t delimiter_branch;
};

/* The parser's state, Linux's `struct hid_parser`. */
struct parser {
    struct hid_desc *desc;
    int fill;       /* the arrays are given: store, not only count */
    uint32_t taken; /* usages the fields have taken */
    uint32_t peak;  /* the most usages the array held at once */
    struct global global;
    struct global global_stack[HID_GLOBAL_STACK_SIZE];
    uint32_t global_stack_ptr;
    struct local local;
    uint16_t collection_stack[HID_MAX_COLLECTIONS];
    uint32_t collection_stack_ptr;
};

uint32_t hid_field_extract(const uint8_t *report, uint32_t offset, uint32_t n)
{
    uint32_t idx = offset / 8;
    uint32_t bit_nr = 0;
    uint32_t bit_shift = offset % 8;
    int bits_to_copy = (int)(8 - bit_shift);
    uint32_t value = 0;
    uint32_t mask;
    int left;

    if (n > 32) {
        n = 32;
    }
    mask = n < 32 ? (1u << n) - 1 : ~0u;
    for (left = (int)n; left > 0; idx++) {
        if (bit_nr < 32) {
            value |= ((uint32_t)report[idx] >> bit_shift) << bit_nr;
        }
        left -= bits_to_copy;
        bit_nr += (uint32_t)bits_to_copy;
        bits_to_copy = 8;
        bit_shift = 0;
    }
    return value & mask;
}

void hid_field_implement(uint8_t *report, uint32_t offset, uint32_t n, uint32_t value)
{
    uint32_t idx = offset / 8;
    uint32_t bit_shift = offset % 8;
    int bits_to_set = (int)(8 - bit_shift);
    int left;

    if (n > 32) {
        n = 32;
    } else if (n < 32) {
        value &= (1u << n) - 1;
    }
    for (left = (int)n; left - bits_to_set >= 0; idx++) {
        report[idx] = (uint8_t)(report[idx] & ~(0xFFu << bit_shift));
        report[idx] = (uint8_t)(report[idx] | (value << bit_shift));
        value = bits_to_set < 32 ? value >> bits_to_set : 0;
        left -= bits_to_set;
        bits_to_set = 8;
        bit_shift = 0;
    }
    if (left) {
        uint32_t bit_mask = (1u << left) - 1;

        report[idx] = (uint8_t)(report[idx] & ~(bit_mask << bit_shift));
        report[idx] = (uint8_t)(report[idx] | (value << bit_shift));
    }
}

int32_t hid_snto32(uint32_t value, uint32_t n)
{
    if (!value || !n) {
        return 0;
    }
    if (n >= 32) {
        return (int32_t)value;
    }
    return (value & (1u << (n - 1))) ? (int32_t)(value | (~0u << n)) : (int32_t)value;
}

struct hid_report *hid_find_report(struct hid_desc *desc, int type, int id)
{
    uint32_t i;

    for (i = 0; i < desc->nreports; i++) {
        if (desc->reports[i].type == type && desc->reports[i].id == id) {
            return &desc->reports[i];
        }
    }
    return 0;
}

uint32_t hid_report_len(const struct hid_report *r)
{
    return (r->size ? ((r->size - 1) >> 3) + 1 : 0) + (r->id > 0 ? 1 : 0);
}

/**
 * Register report `id` of kind `type` in `application`, or find it
 * registered already, as Linux's `hid_register_report`. Returns its index,
 * or -1 when the table is full.
 */
static int register_report(struct hid_desc *desc, unsigned type, uint32_t id, uint32_t application)
{
    struct hid_report *r = hid_find_report(desc, (int)type, (int)id);

    if (r) {
        return (int)(r - desc->reports);
    }
    if (desc->nreports == HID_MAX_REPORTS) {
        return -1;
    }
    r = &desc->reports[desc->nreports];
    r->type = (uint8_t)type;
    r->id = (uint8_t)id;
    r->size = 0;
    r->application = application;
    if (id != 0) {
        desc->numbered[type] = 1;
    }
    return (int)desc->nreports++;
}

/** Open a collection of `type`, whose usage is the first local usage, as
 * Linux's `open_collection`. */
static int open_collection(struct parser *p, unsigned type)
{
    struct hid_desc *desc = p->desc;
    uint32_t index = desc->ncollections;

    if (p->collection_stack_ptr == HID_MAX_COLLECTIONS || index > 0xFFFFu) {
        return -E2BIG;
    }
    if (p->fill) {
        struct hid_collection *c;

        if (index >= desc->max_collections) {
            return -E2BIG;
        }
        c = &desc->collections[index];
        c->type = (uint8_t)type;
        c->usage = p->local.usage_index ? desc->usages[p->taken].hid : 0;
        c->level = (uint16_t)p->collection_stack_ptr;
        c->parent = (int16_t)(p->collection_stack_ptr == 0
                                  ? -1
                                  : (int)p->collection_stack[p->collection_stack_ptr - 1]);
    }
    p->collection_stack[p->collection_stack_ptr++] = (uint16_t)index;
    desc->ncollections++;
    if (type == HID_COLLECTION_APPLICATION) {
        desc->maxapplication++;
    }
    return 0;
}

/** The usage of the innermost open collection of `type`, or 0, Linux's
 * `hid_lookup_collection`. Always 0 while only counting. */
static uint32_t lookup_collection(struct parser *p, unsigned type)
{
    int n;

    if (!p->fill) {
        return 0;
    }
    for (n = (int)p->collection_stack_ptr - 1; n >= 0; n--) {
        struct hid_collection *c = &p->desc->collections[p->collection_stack[n]];

        if (c->type == type) {
            return c->usage;
        }
    }
    return 0;
}

/** The local usage `i` with the current usage page in its high half,
 * Linux's `complete_usage`. */
static void complete_usage(struct parser *p, uint32_t i)
{
    struct hid_usage *u = &p->desc->usages[p->taken + i];

    u->hid = (u->hid & 0xFFFFu) | ((p->global.usage_page & 0xFFFFu) << 16);
}

/** Add a local usage, its Usage item `size` bytes, as Linux's
 * `hid_add_usage`. */
static int add_usage(struct parser *p, uint32_t usage, unsigned size)
{
    uint32_t at;

    if (p->local.usage_index >= HID_MAX_USAGES) {
        return -EINVAL;
    }
    at = p->taken + p->local.usage_index;
    if (p->fill) {
        struct hid_usage *u;

        if (at >= p->desc->max_usages) {
            return -E2BIG;
        }
        u = &p->desc->usages[at];
        k_memset(u, 0, sizeof(*u));
        u->hid = usage;
        u->size = (uint8_t)size;
        u->collection =
            (uint16_t)(p->collection_stack_ptr ? p->collection_stack[p->collection_stack_ptr - 1]
                                               : 0);
        if (size <= 2) {
            complete_usage(p, p->local.usage_index);
        }
    }
    p->local.usage_index++;
    if (at + 1 > p->peak) {
        p->peak = at + 1;
    }
    return 0;
}

/**
 * Make a field of the main item of kind `type` with data `flags`, as
 * Linux's `hid_add_field`: the report grows by the item's bits whether or
 * not a field is made, and an item with no usage is padding.
 */
static int add_field(struct parser *p, unsigned type, uint32_t flags)
{
    struct hid_desc *desc = p->desc;
    struct global *g = &p->global;
    uint32_t application = lookup_collection(p, HID_COLLECTION_APPLICATION);
    struct hid_report *r;
    uint32_t usages, offset, i, report_fields;
    int index;

    index = register_report(desc, type, g->report_id, application);
    if (index < 0) {
        return -E2BIG;
    }
    r = &desc->reports[index];
    if ((g->logical_minimum < 0 && g->logical_maximum < g->logical_minimum) ||
        (g->logical_minimum >= 0 && (uint32_t)g->logical_maximum < (uint32_t)g->logical_minimum)) {
        return -EINVAL;
    }
    offset = r->size;
    r->size += g->report_size * g->report_count;
    if (r->size > (uint32_t)(HID_MAX_BUFFER_SIZE - 1) << 3) {
        return -EINVAL;
    }
    if (!p->local.usage_index) {
        return 0;
    }
    usages = p->local.usage_index > g->report_count ? p->local.usage_index : g->report_count;

    report_fields = 0;
    for (i = 0; i < desc->nfields; i++) {
        if (!p->fill || desc->fields[i].report == (uint16_t)index) {
            report_fields++;
        }
    }
    /* While counting, every field so far is counted against this report,
     * which can only make the arrays larger than the filling uses. */
    if (p->fill && report_fields == HID_MAX_FIELDS) {
        return 0;
    }

    if (p->fill) {
        struct hid_field *f;

        if (desc->nfields >= desc->max_fields || p->taken + usages > desc->max_usages) {
            return -E2BIG;
        }
        f = &desc->fields[desc->nfields];
        k_memset(f, 0, sizeof(*f));
        f->report = (uint16_t)index;
        f->input = HID_NO_INPUT;
        f->physical = lookup_collection(p, HID_COLLECTION_PHYSICAL);
        f->logical = lookup_collection(p, HID_COLLECTION_LOGICAL);
        f->application = application;
        for (i = p->local.usage_index; i < usages; i++) {
            desc->usages[p->taken + i] = desc->usages[p->taken + p->local.usage_index - 1];
        }
        for (i = 0; i < usages; i++) {
            desc->usages[p->taken + i].size = 0;
            desc->usages[p->taken + i].value = 0;
            desc->usages[p->taken + i].new_value = 0;
            desc->usages[p->taken + i].multiplier = 1;
        }
        f->usage = p->taken;
        f->maxusage = usages;
        f->flags = flags;
        f->offset = offset;
        f->size = g->report_size;
        f->count = g->report_count;
        f->logical_minimum = g->logical_minimum;
        f->logical_maximum = g->logical_maximum;
        f->physical_minimum = g->physical_minimum;
        f->physical_maximum = g->physical_maximum;
        f->unit_exponent = g->unit_exponent;
        f->unit = g->unit;
    }
    desc->nfields++;
    p->taken += usages;
    if (p->taken > p->peak) {
        p->peak = p->taken;
    }
    return 0;
}

/** The data of `it` as unsigned, Linux's `item_udata`. */
static uint32_t item_udata(const struct item *it)
{
    return it->data;
}

/** The data of `it` sign-extended from its size, Linux's `item_sdata`. */
static int32_t item_sdata(const struct item *it)
{
    switch (it->size) {
    case 1:
        return (int32_t)(int8_t)it->data;
    case 2:
        return (int32_t)(int16_t)it->data;
    case 4:
        return (int32_t)it->data;
    default:
        return 0;
    }
}

/** A global item, as Linux's `hid_parser_global`. */
static int parse_global(struct parser *p, const struct item *it)
{
    struct global *g = &p->global;
    int32_t raw;

    switch (it->tag) {
    case GLOBAL_PUSH:
        if (p->global_stack_ptr == HID_GLOBAL_STACK_SIZE) {
            return -EINVAL;
        }
        p->global_stack[p->global_stack_ptr++] = *g;
        return 0;
    case GLOBAL_POP:
        if (!p->global_stack_ptr) {
            return -EINVAL;
        }
        *g = p->global_stack[--p->global_stack_ptr];
        return 0;
    case GLOBAL_USAGE_PAGE:
        g->usage_page = item_udata(it);
        return 0;
    case GLOBAL_LOGICAL_MIN:
        g->logical_minimum = item_sdata(it);
        return 0;
    case GLOBAL_LOGICAL_MAX:
        g->logical_maximum = g->logical_minimum < 0 ? item_sdata(it) : (int32_t)item_udata(it);
        return 0;
    case GLOBAL_PHYSICAL_MIN:
        g->physical_minimum = item_sdata(it);
        return 0;
    case GLOBAL_PHYSICAL_MAX:
        g->physical_maximum = g->physical_minimum < 0 ? item_sdata(it) : (int32_t)item_udata(it);
        return 0;
    case GLOBAL_UNIT_EXPONENT:
        /* Many devices give the exponent as a 4-bit two's complement
         * nibble, which Linux takes beside the standard encoding. */
        raw = item_sdata(it);
        g->unit_exponent = !((uint32_t)raw & 0xFFFFFFF0u) ? hid_snto32((uint32_t)raw, 4) : raw;
        return 0;
    case GLOBAL_UNIT:
        g->unit = item_udata(it);
        return 0;
    case GLOBAL_REPORT_SIZE:
        g->report_size = item_udata(it);
        return g->report_size > 256 ? -EINVAL : 0;
    case GLOBAL_REPORT_COUNT:
        g->report_count = item_udata(it);
        return g->report_count > HID_MAX_USAGES ? -EINVAL : 0;
    case GLOBAL_REPORT_ID:
        g->report_id = item_udata(it);
        return (g->report_id == 0 || g->report_id >= HID_MAX_IDS) ? -EINVAL : 0;
    default:
        return -EINVAL;
    }
}

/** A local item, as Linux's `hid_parser_local`. */
static int parse_local(struct parser *p, const struct item *it)
{
    uint32_t data = item_udata(it);
    uint32_t n, count;
    int err;

    switch (it->tag) {
    case LOCAL_DELIMITER:
        /* The usages before the first delimiter, and the first set, are
         * taken; the alternatives after it are not. */
        if (data) {
            if (p->local.delimiter_depth != 0) {
                return -EINVAL;
            }
            p->local.delimiter_depth++;
            p->local.delimiter_branch++;
        } else {
            if (p->local.delimiter_depth < 1) {
                return -EINVAL;
            }
            p->local.delimiter_depth--;
        }
        return 0;
    case LOCAL_USAGE:
        if (p->local.delimiter_branch > 1) {
            return 0;
        }
        return add_usage(p, data, it->size);
    case LOCAL_USAGE_MINIMUM:
        if (p->local.delimiter_branch > 1) {
            return 0;
        }
        p->local.usage_minimum = data;
        return 0;
    case LOCAL_USAGE_MAXIMUM:
        if (p->local.delimiter_branch > 1) {
            return 0;
        }
        count = data - p->local.usage_minimum;
        if (count + p->local.usage_index >= HID_MAX_USAGES) {
            /* Linux cuts the range at the table's end. */
            data = HID_MAX_USAGES - p->local.usage_index + p->local.usage_minimum - 1;
            if (data == 0) {
                return -EINVAL;
            }
        }
        for (n = p->local.usage_minimum; n <= data; n++) {
            err = add_usage(p, n, it->size);
            if (err) {
                return err;
            }
            if (n == 0xFFFFFFFFu) {
                break;
            }
        }
        return 0;
    default:
        return 0;
    }
}

/**
 * Give the usages declared before the last Usage Page that page, from the
 * last back to the first one that has it already, as the specification's
 * 6.2.2.8 says, Linux's `hid_concatenate_last_usage_page`.
 */
static void concatenate_last_usage_page(struct parser *p)
{
    int i;

    if (!p->fill) {
        return;
    }
    for (i = (int)p->local.usage_index - 1; i >= 0; i--) {
        struct hid_usage *u = &p->desc->usages[p->taken + (uint32_t)i];

        if (u->size > 2) {
            continue;
        }
        if ((u->hid >> 16) == (p->global.usage_page & 0xFFFFu)) {
            break;
        }
        complete_usage(p, (uint32_t)i);
    }
}

/** A main item, as Linux's `hid_parser_main`; the local state ends here. */
static int parse_main(struct parser *p, const struct item *it)
{
    uint32_t data = item_udata(it);
    int ret;

    concatenate_last_usage_page(p);
    switch (it->tag) {
    case MAIN_COLLECTION:
        ret = open_collection(p, data & 0xFFu);
        break;
    case MAIN_END_COLLECTION:
        if (!p->collection_stack_ptr) {
            ret = -EINVAL;
        } else {
            p->collection_stack_ptr--;
            ret = 0;
        }
        break;
    case MAIN_INPUT:
        ret = add_field(p, HID_INPUT_REPORT, data);
        break;
    case MAIN_OUTPUT:
        ret = add_field(p, HID_OUTPUT_REPORT, data);
        break;
    case MAIN_FEATURE:
        ret = add_field(p, HID_FEATURE_REPORT, data);
        break;
    default:
        ret = 0; /* Linux warns and goes on */
        break;
    }
    k_memset(&p->local, 0, sizeof(p->local));
    return ret;
}

/**
 * Read the item at `*at`, before `end`, into `it`, as Linux's
 * `fetch_item`. Returns 1 with `*at` past it, 0 when the item is cut short
 * or there is none, and -1 for a long item, which Linux refuses.
 */
static int fetch_item(const uint8_t **at, const uint8_t *end, struct item *it)
{
    const uint8_t *s = *at;
    uint8_t b;

    if (end - s <= 0) {
        return 0;
    }
    b = *s++;
    it->type = (b >> 2) & 3u;
    it->tag = (b >> 4) & 15u;
    if (it->tag == TAG_LONG) {
        if (end - s < 2) {
            return 0;
        }
        it->size = *s++;
        s++;
        if (end - s < (long)it->size) {
            return 0;
        }
        *at = s + it->size;
        return -1;
    }
    it->size = b & 3u;
    if (it->size == 3) {
        it->size = 4;
    }
    if (end - s < (long)it->size) {
        return 0;
    }
    switch (it->size) {
    case 0:
        it->data = 0;
        break;
    case 1:
        it->data = s[0];
        break;
    case 2:
        it->data = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
        break;
    default:
        it->data = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) |
                   ((uint32_t)s[3] << 24);
        break;
    }
    *at = s + it->size;
    return 1;
}

int hid_parse(struct hid_desc *desc, const uint8_t *rdesc, int length)
{
    struct parser parser;
    struct parser *p = &parser;
    struct hid_field *fields = desc->fields;
    struct hid_usage *usages = desc->usages;
    struct hid_collection *collections = desc->collections;
    uint32_t max_fields = desc->max_fields;
    uint32_t max_usages = desc->max_usages;
    uint32_t max_collections = desc->max_collections;
    const uint8_t *at = rdesc;
    const uint8_t *end;
    struct item it;
    int got, err;

    k_memset(desc, 0, sizeof(*desc));
    desc->fields = fields;
    desc->usages = usages;
    desc->collections = collections;
    desc->max_fields = max_fields;
    desc->max_usages = max_usages;
    desc->max_collections = max_collections;
    k_memset(p, 0, sizeof(*p));
    p->desc = desc;
    p->fill = fields != 0;
    if (p->fill && (!usages || (!collections && max_collections))) {
        return -E2BIG;
    }
    if (!rdesc || length <= 0) {
        return -EINVAL;
    }
    end = rdesc + length;

    while ((got = fetch_item(&at, end, &it)) != 0) {
        if (got < 0) {
            return -EINVAL;
        }
        switch (it.type) {
        case ITEM_MAIN:
            err = parse_main(p, &it);
            break;
        case ITEM_GLOBAL:
            err = parse_global(p, &it);
            break;
        case ITEM_LOCAL:
            err = parse_local(p, &it);
            break;
        default:
            err = 0;
            break;
        }
        if (err) {
            return err;
        }
        if (at == end) {
            if (p->collection_stack_ptr || p->local.delimiter_depth) {
                return -EINVAL;
            }
            desc->nusages = p->taken;
            if (!p->fill) {
                desc->max_fields = desc->nfields;
                desc->max_usages = p->peak;
                desc->max_collections = desc->ncollections;
            }
            return 0;
        }
    }
    return -EINVAL;
}
