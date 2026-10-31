/*
 * keymap.c: the keyboard's translator, from key codes to text and terminal
 * sequences through keymaps in Linux's encoding (keymap.h).
 */
#include "keymap.h"

const unsigned short *const keymap_default_tables[KM_MAX_TABLES] = {0};

const char *const keymap_default_func[KM_MAX_FUNC] = {0};

/* One entry, since C89 has no array of none; `keymap_default_ndiacr` says
 * how many are used. */
const struct keymap_diacr keymap_default_diacr[1] = {{0, 0, 0}};

const unsigned int keymap_default_ndiacr = 0;

void keymap_state_init(struct keymap_state *st, unsigned int ledflags)
{
    unsigned int i;

    for (i = 0; i < KM_NR_SHIFT; i++) {
        st->down[i] = 0;
    }
    st->shift = 0;
    st->locks = 0;
    st->slocks = 0;
    st->ledflags = ledflags;
    st->diacr = 0;
    st->compose = 0;
    st->npad = 0;
    st->npad_on = 0;
}

int keymap_key(struct keymap_state *st, const struct keymap *map, unsigned int keycode, int value,
               struct keymap_result *res)
{
    (void)st;
    (void)map;
    (void)keycode;
    res->kind = KM_RESULT_NONE;
    res->ntext = 0;
    res->func = 0;
    res->key = 0;
    res->mods = 0;
    res->ledflags = st->ledflags;
    res->repeat = value == 2;
    return KM_RESULT_NONE;
}

unsigned int keymap_encode(const struct keymap *map, const struct keymap_result *res,
                           unsigned int modes, char *buf, unsigned int size)
{
    (void)map;
    (void)res;
    (void)modes;
    (void)buf;
    (void)size;
    return 0;
}

unsigned int keymap_default_entry(const void *ctx, unsigned int table, unsigned int keycode)
{
    const unsigned short *const *tables = (const unsigned short *const *)ctx;

    if (table >= KM_MAX_TABLES || tables[table] == 0) {
        return KM_NOSUCHMAP;
    }
    if (keycode >= KM_NR_KEYS) {
        return KM_HOLE;
    }
    return tables[table][keycode];
}
