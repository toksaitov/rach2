/*
 * hdmi_audio_linux_hosttest.c: Haswell's display audio (audio/hda_hdmi.c)
 * where Linux's snd_hda_intel, OSS layer and ALSA core set the behaviour,
 * on the controller, codec and display models of hdmi_audio_hosttest.c.
 *
 * host-deps: rach2/sys/lib/kstring.c tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The rows: a controller the firmware left in D3hot is brought to D0, its
 * header kept, before the bind enables its decoding; a refill late by a
 * whole lap of the ring counts the lap, as Linux's snd_pcm_update_hw_ptr0
 * counts jiffies, and one a tick late counts none; OSS's default, unsigned
 * 8-bit mono at 8 kHz, is granted and converted to 16-bit stereo at the
 * sink's 32 kHz, the samples moved to signed and linearly interpolated, as
 * Linux's OSS plugins do; and the display's going stops the stream, gives
 * the display's power back, tells the stream's owner, and leaves a stream
 * that cannot start again once the display is back, whose close gives
 * nothing more back.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define main hdmi_audio_rows
#include "hdmi_audio_hosttest.c"
#undef main

/* The bytes the converting rows' transfer gives: the first, then the rest. */
static uint8_t first_byte, rest_byte;
static uint32_t given;
static int gones;

/** A transfer of `first_byte` then `rest_byte`, counting the bytes. */
static void ramp_transfer(audio_stream_t *s, uint8_t *buf, size_t size)
{
    (void)s;
    k_memset(buf, rest_byte, size);
    if (given == 0 && size > 0) {
        buf[0] = first_byte;
    }
    given += (uint32_t)size;
}

/** The stream's gone, counted. */
static void s_gone(audio_stream_t *s)
{
    (void)s;
    gones++;
}

/** The 16-bit sample `i` of the DMA buffer. */
static int dma16(uint32_t i)
{
    return (int16_t)((uint16_t)hh_dma[2 * i] | (uint16_t)hh_dma[2 * i + 1] << 8);
}

/** The firmware's D3hot: the bind finds the controller in D0 again. */
static void row_d0(void)
{
    pci_d_state = PCI_D3HOT;
    row_bind();
    check(pci_d_state == PCI_D0 && pci_saved && pci_restored,
          "d0: D3hot left for D0, the header saved and written back");
    check(pci_enabled_asleep == 0, "d0: no decoding enabled before D0");
}

/** A refill late by a lap of the ring, and one a tick late. */
static void row_laps(void)
{
    audio_stream_t s;
    uint32_t ring_ticks = 40u * TICK_HZ / 1000u;

    stream_init(&s, 48000, AUDIO_S16LE, 128);
    check(hh_stream_open(&s) == 0 && hh_stream_setup(&s) == 0 && hh_copy && hh_pb == 128 &&
              hh_n == 60,
          "laps: a ring of 60 periods of 128 bytes, 40 ms");
    now_ticks = 1000;
    transfers = 0;
    periods = 0;
    check(hh_stream_start(&s) == 0 && transfers == 60, "laps: the start fills the ring");
    now_ticks += ring_ticks;
    mm_put(hh_sd(HDA_SD_LPIB), 0, 32);
    hdmi_audio_tick();
    check(transfers == 120 && periods == 60 && hh_stream_position(&s) == 1920,
          "laps: a tick 40 ms late refills the ring and counts its 1920 frames");
    now_ticks += 1;
    mm_put(hh_sd(HDA_SD_LPIB), 6 * 128, 32);
    check(hh_stream_position(&s) == 1920 + 192, "laps: the position reads the lap-free gap");
    hdmi_audio_tick();
    check(transfers == 126 && periods == 66, "laps: a tick 4 ms on refills the 6 periods played");
    hh_stream_close(&s);
    check(display_power == 0, "laps: the close gives the display's power back");
}

/** OSS's default format converted, and a ramp interpolated. */
static void row_convert(void)
{
    audio_stream_t s;
    int i;

    stream_init(&s, 8000, AUDIO_U8, 128);
    s.channels = 1;
    s.transfer = ramp_transfer;
    check(hh_stream_open(&s) == 0 && hh_stream_setup(&s) == 0 && s.format == AUDIO_U8 &&
              s.channels == 1 && s.rate == 8000 && s.period_bytes == 128,
          "convert: unsigned 8-bit mono at 8 kHz granted");
    check(!hh_copy && hh_rate == 32000 && hh_frame == 4 && hh_fmt == 0x0a11 && hh_pb == 2048 &&
              hh_n == 3 && s.device_periods == 4,
          "convert: the DMA in 16-bit stereo at 32 kHz, three periods of 2048 bytes");
    first_byte = 0x80;
    rest_byte = 0xc0;
    given = 0;
    check(hh_stream_start(&s) == 0 && given == 385 && hh_taken == 385,
          "convert: the start takes the 385 frames its 1536 need");
    for (i = 0; i < 5; i++) {
        check(dma16(2u * (uint32_t)i) == 0x1000 * i && dma16(2u * (uint32_t)i + 1u) == 0x1000 * i,
              "convert: 0 to 0x4000 in four steps, on both channels");
    }
    check(dma16(2 * 1535) == 0x4000, "convert: the last frame is 0xc0's 0x4000");
    hh_stream_close(&s);
}

/** The display goes under an open stream, and comes back. */
static void row_gone(void)
{
    audio_stream_t s;

    stream_init(&s, 48000, AUDIO_S16LE, 4096);
    s.gone = s_gone;
    gones = 0;
    check(hh_stream_open(&s) == 0 && hh_stream_setup(&s) == 0 && hh_stream_start(&s) == 0 &&
              display_power == 1,
          "gone: a stream plays, holding the display's power");
    acomp_unregister_display(&disp_dev);
    check(gones == 1 && !hh_running && display_power == 0 && !hdmi_audio_present(),
          "gone: the display's going stops it, gives the power back and tells its owner");
    check(acomp_register_display(&disp_ops, &disp_dev) == 0 && hdmi_audio_present() &&
              display_power == 0,
          "gone: the display comes back");
    check(hh_stream_start(&s) == -EIO && hh_stream_open(&s) == -EBUSY,
          "gone: the old stream cannot start, and holds the device until its close");
    hh_stream_close(&s);
    check(display_power == 0 && !hh_stream, "gone: its close gives nothing more back");
    stream_init(&s, 48000, AUDIO_S16LE, 4096);
    check(hh_stream_open(&s) == 0 && display_power == 1, "gone: a new open holds the power");
    hh_stream_close(&s);
    check(display_power == 0, "gone: and gives it back");
}

int main(void)
{
    row_d0();
    row_laps();
    row_convert();
    row_gone();
    if (failures == 0) {
        printf("hdmi-audio-linux-test: ok\n");
    }
    return failures != 0;
}
