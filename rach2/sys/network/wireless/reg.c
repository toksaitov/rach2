/* The 2.4 GHz world domain, narrowed by a radio's EEPROM country. */
#include "wiphy.h"

/** Intersect the radio channels with world and known EEPROM restrictions. */
void wlan_reg_apply(struct wlan_band *band, const char country[2])
{
    unsigned i;
    int us = country[0] == 'U' && country[1] == 'S';
    int ca = country[0] == 'C' && country[1] == 'A';

    for (i = 0; i < band->n_channels; ++i) {
        struct wlan_channel *c = &band->channels[i];
        if (c->number < 1 || c->number > 13 || c->frequency != 2407u + 5u * c->number) {
            c->flags |= WLAN_CHANNEL_DISABLED;
        } else if (c->number > 11) {
            c->flags |= WLAN_CHANNEL_PASSIVE;
            if (us || ca)
                c->flags |= WLAN_CHANNEL_DISABLED;
        }
        if (c->max_power > 20)
            c->max_power = 20;
    }
}

/** Find an allowed frequency, excluding passive channels for active use. */
const struct wlan_channel *wlan_channel(const struct wlan *wlan, unsigned frequency, int active)
{
    unsigned i;

    for (i = 0; wlan && i < wlan->band.n_channels; ++i) {
        const struct wlan_channel *c = &wlan->band.channels[i];
        if (c->frequency == frequency && !(c->flags & WLAN_CHANNEL_DISABLED) &&
            (!active || !(c->flags & WLAN_CHANNEL_PASSIVE)))
            return c;
    }
    return 0;
}
