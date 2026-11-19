/*
 * asound.c: /proc/asound's cards and card directories, as Linux 6.12.115's
 * ALSA core lists them (sound/core/init.c, snd_card_info_read), over the
 * analog HD Audio card of hda.c and the HDMI codec's card of hda_hdmi.c.
 */

#include "asound.h"

#include "hda.h"
#include "hda_hdmi.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../pci.h"

/* A text written into a caller's buffer: what fits is stored, and `len`
 * counts every byte, so a caller learns the size a short buffer lacked. */
struct asound_text {
    char *buf;
    int cap;
    int len;
};

/*
 * The functions Linux's snd_hda_intel names "HDA Intel PCH" (AZX_DRIVER_PCH
 * and AZX_DRIVER_SKL in sound/pci/hda/hda_intel.c's azx_ids): Ibex Peak to
 * Comet Lake, Bay Trail, Braswell and the Atom SoCs after them. Another
 * Intel function is "HDA Intel", as the table's class match for Intel makes
 * it.
 */
static const unsigned short asound_pch[] = {
    0x0f04, 0x2284, 0x3b56, 0x3b57, 0x1c20, 0x1d20, 0x1e20, 0x8c20, 0x8c21, 0x8ca0, 0x8d20, 0x8d21,
    0x9c20, 0x9c21, 0x9ca0, 0xa1f0, 0xa270, 0xa170, 0x9d70, 0xa171, 0x9d71, 0xa2f0, 0xa348, 0x9dc8,
    0x02c8, 0x06c8, 0xa3f0, 0x34c8, 0x38c8, 0x4dc8, 0xa0c8, 0x43c8, 0x5a98, 0x1a98, 0x3198};

/** Append the string `s`. */
static void as_put_s(struct asound_text *t, const char *s)
{
    while (*s) {
        if (t->buf && t->len < t->cap) {
            t->buf[t->len] = *s;
        }
        t->len++;
        s++;
    }
}

/** Append `v` in base `base`, 10 or 16; base 10 takes the low 32 bits, so
 * that i686 needs no 64-bit division. */
static void as_put_n(struct asound_text *t, uint64_t v, unsigned base)
{
    char d[20], c[2];
    uint32_t w = (uint32_t)v;
    int n = 0;

    do {
        if (base == 16) {
            d[n++] = "0123456789abcdef"[v & 15u];
            v >>= 4;
        } else {
            d[n++] = (char)('0' + w % 10u);
            w /= 10u;
        }
    } while (base == 16 ? v != 0 : w != 0);
    c[1] = '\0';
    while (n > 0) {
        c[0] = d[--n];
        as_put_s(t, c);
    }
}

/** The short name Linux's snd_hda_intel gives the controller `pci`
 * (driver_short_names). */
static const char *asound_short(const pci_device_t *pci)
{
    unsigned i;

    if (pci->vendor_id == 0x8086) {
        for (i = 0; i < sizeof(asound_pch) / sizeof(asound_pch[0]); i++) {
            if (pci->device_id == asound_pch[i]) {
                return "HDA Intel PCH";
            }
        }
        return "HDA Intel";
    }
    return pci->vendor_id == 0x10de ? "HDA NVidia" : "HD-Audio Generic";
}

/** The card's id, the last word of its short name, as Linux's
 * retrieve_id_from_card_name takes it when the driver sets none. */
static const char *asound_id(const char *shortname)
{
    const char *id = shortname;

    for (; *shortname; shortname++) {
        if (*shortname == ' ' && shortname[1]) {
            id = shortname + 1;
        }
    }
    return id;
}

/** The HDMI card's number, or -1 while it is not there. */
static int asound_hdmi_card(void)
{
    return hdmi_audio_present() ? hda_card(NULL) : -1;
}

int asound_card_count(void)
{
    return hda_card(NULL) + (hdmi_audio_present() ? 1 : 0);
}

/** Append "eld#C.N" for the HDMI card's ELD file N. */
static void as_put_eld_name(struct asound_text *t, int n)
{
    as_put_s(t, "eld#");
    as_put_n(t, (uint32_t)hdmi_audio_codec_addr(), 10);
    as_put_s(t, ".");
    as_put_n(t, (uint32_t)n, 10);
}

int asound_name(int file, char *name, int cap)
{
    struct asound_text t;
    int card = file / ASOUND_PER_CARD, f = file % ASOUND_PER_CARD;

    t.buf = name;
    t.cap = cap;
    t.len = 0;
    if (file == ASOUND_CARDS) {
        as_put_s(&t, "cards");
    } else if (file < 0 || card >= asound_card_count() || f == ASOUND_CARDS) {
        return -1;
    } else if (f == ASOUND_ID) {
        as_put_s(&t, "id");
    } else if (card == asound_hdmi_card() && f - ASOUND_ELD < hdmi_audio_eld_count()) {
        as_put_eld_name(&t, f - ASOUND_ELD);
    } else {
        return -1;
    }
    if (t.len >= cap) {
        return -1;
    }
    name[t.len] = '\0';
    return 0;
}

int asound_entry(int card, int n)
{
    if (card < 0 || card >= asound_card_count() || n < 0) {
        return -1;
    }
    if (n == 0) {
        return ASOUND_FILE(card, ASOUND_ID);
    }
    if (card == asound_hdmi_card() && n - 1 < hdmi_audio_eld_count() &&
        n - 1 < ASOUND_PER_CARD - ASOUND_ELD) {
        return ASOUND_FILE(card, ASOUND_ELD + n - 1);
    }
    return -1;
}

int asound_lookup(int card, const char *name, int len)
{
    char have[24];
    int n, file, i;

    for (n = 0; (file = asound_entry(card, n)) >= 0; n++) {
        if (asound_name(file, have, (int)sizeof(have)) != 0) {
            continue;
        }
        if ((int)k_strlen(have) != len) {
            continue;
        }
        for (i = 0; i < len && have[i] == name[i]; i++) {
            ;
        }
        if (i == len) {
            return file;
        }
    }
    return -1;
}

/** Append the analog card's two lines of /proc/asound/cards, numbered 0:
 * snd_card_info_read's "%2i [%-15s]: %s - %s" and azx_probe_continue's
 * long name, "%s at 0x%lx irq %i". */
static void as_put_analog(struct asound_text *t, const pci_device_t *pci)
{
    const char *shortname = asound_short(pci), *id = asound_id(shortname);
    int pad;

    as_put_s(t, " 0 [");
    as_put_s(t, id);
    for (pad = (int)k_strlen(id); pad < 15; pad++) {
        as_put_s(t, " ");
    }
    as_put_s(t, "]: HDA-Intel - ");
    as_put_s(t, shortname);
    as_put_s(t, "\n                      ");
    as_put_s(t, shortname);
    as_put_s(t, " at 0x");
    as_put_n(t, pci_bar_address64(pci, 0), 16);
    as_put_s(t, " irq ");
    as_put_n(t, pci->irq, 10);
    as_put_s(t, "\n");
}

int asound_text(int file, char *buf, int cap)
{
    struct asound_text t;
    pci_device_t pci;
    int card = file / ASOUND_PER_CARD, f = file % ASOUND_PER_CARD, hdmi = asound_hdmi_card();
    int analog = hda_card(&pci), n;

    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    if (file == ASOUND_CARDS) {
        if (analog) {
            as_put_analog(&t, &pci);
        }
        n = hdmi < 0 ? -ENOENT
                     : hdmi_audio_proc_text(0, hdmi, buf && t.len < cap ? buf + t.len : NULL,
                                            cap - t.len < 0 ? 0 : cap - t.len);
        if (n > 0) {
            t.len += n;
        } else if (!analog) {
            as_put_s(&t, "--- no soundcards ---\n");
        }
        return t.len;
    }
    if (file < 0 || f == ASOUND_CARDS) {
        return -ENOENT;
    }
    if (card == 0 && analog) {
        if (f != ASOUND_ID) {
            return -ENOENT;
        }
        as_put_s(&t, asound_id(asound_short(&pci)));
        as_put_s(&t, "\n");
        return t.len;
    }
    if (card != hdmi) {
        return -ENOENT;
    }
    return hdmi_audio_proc_text(f, card, buf, cap);
}

int asound_mode(int file)
{
    return file > 0 && file % ASOUND_PER_CARD >= ASOUND_ELD ? 0644 : 0444;
}

int asound_write(int file, const char *text, int len)
{
    if (asound_mode(file) != 0644) {
        return -EACCES;
    }
    if (file / ASOUND_PER_CARD != asound_hdmi_card()) {
        return -ENOENT;
    }
    return hdmi_audio_eld_write(file % ASOUND_PER_CARD - ASOUND_ELD, text, len);
}
