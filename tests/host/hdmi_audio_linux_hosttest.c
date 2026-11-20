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
 * header kept, before the bind enables its decoding; a refill late by a whole
 * lap of the ring counts the lap, as Linux's snd_pcm_update_hw_ptr0 counts
 * jiffies, and one a tick late counts none; OSS's default, unsigned 8-bit mono
 * at 8 kHz, is granted and converted to 16-bit stereo at the sink's 32 kHz, the
 * samples moved to signed and linearly interpolated, as Linux's OSS plugins do;
 * and the display's going stops the stream, gives the display's power back,
 * tells the stream's owner, and leaves a stream that cannot start again once
 * the display is back, which a monitor's notification gives no pin and whose
 * close gives nothing more back. /proc/asound numbers the PCH's analog card 0
 * and the HDMI card 1, with Linux's names and ids, and root's write to an ELD
 * file sets the fields Linux's snd_hdmi_write_eld_info sets, sadN_max_bitrate
 * among them, its reads and the open's limit by the sink bounded where Linux's
 * index past their tables, a count of 7fffffff among them.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <string.h>

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
    return (int16_t)((uint16_t)hh_dma[0][2 * i] | (uint16_t)hh_dma[0][2 * i + 1] << 8);
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
    check(hh_open0(&s) == 0 && hh_stream_setup(&s) == 0 && M0->copy && M0->pb == 128 && M0->n == 60,
          "laps: a ring of 60 periods of 128 bytes, 40 ms");
    now_ticks = 1000;
    transfers = 0;
    periods = 0;
    check(hh_stream_start(&s) == 0 && transfers == 60, "laps: the start fills the ring");
    now_ticks += ring_ticks;
    mm_put(hh_sd(M0, HDA_SD_LPIB), 0, 32);
    hdmi_audio_tick();
    check(transfers == 120 && periods == 60 && hh_stream_position(&s) == 1920,
          "laps: a tick 40 ms late refills the ring and counts its 1920 frames");
    now_ticks += 1;
    mm_put(hh_sd(M0, HDA_SD_LPIB), 6 * 128, 32);
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
    check(hh_open0(&s) == 0 && hh_stream_setup(&s) == 0 && s.format == AUDIO_U8 &&
              s.channels == 1 && s.rate == 8000 && s.period_bytes == 128,
          "convert: unsigned 8-bit mono at 8 kHz granted");
    check(!M0->copy && M0->rate == 32000 && M0->frame == 4 && M0->fmt == 0x0a11 && M0->pb == 2048 &&
              M0->n == 3 && s.device_periods == 4,
          "convert: the DMA in 16-bit stereo at 32 kHz, three periods of 2048 bytes");
    first_byte = 0x80;
    rest_byte = 0xc0;
    given = 0;
    check(hh_stream_start(&s) == 0 && given == 385 && M0->taken == 385,
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
    check(hh_open0(&s) == 0 && hh_stream_setup(&s) == 0 && hh_stream_start(&s) == 0 &&
              display_power == 1,
          "gone: a stream plays, holding the display's power");
    acomp_unregister_display(&disp_dev);
    check(gones == 1 && !M0->running && display_power == 0 && !hdmi_audio_present(),
          "gone: the display's going stops it, gives the power back and tells its owner");
    check(acomp_register_display(&disp_ops, &disp_dev) == 0 && hdmi_audio_present() &&
              display_power == 0,
          "gone: the display comes back");
    acomp_eld_notify(1, -1);
    check(M0->pin == 0 && hh_pins[0].pcm == 0 && hh_pins[0].cvt == 0 && M0->cvt_idx < 0,
          "gone: a notification after the return gives the old stream no pin");
    check(hh_stream_start(&s) == -EIO && hh_open0(&s) == -EBUSY,
          "gone: the old stream cannot start, and holds the device until its close");
    hh_stream_close(&s);
    check(display_power == 0 && !M0->stream, "gone: its close gives nothing more back");
    stream_init(&s, 48000, AUDIO_S16LE, 4096);
    check(hh_open0(&s) == 0 && display_power == 1, "gone: a new open holds the power");
    hh_stream_close(&s);
    check(display_power == 0, "gone: and gives it back");
}

/** The PCH's analog controller of a Haswell laptop, 8086:9C20 at 00:1b.0. */
static const pci_device_t pch = {0x8086, 0x9c20, 4, 3, 0, 4, 0, 0x1b, 0, 10, {0}};

/** The analog card numbered 0 and the HDMI card 1, with Linux's names. */
static void row_cards(void)
{
    static const char cards[] = " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n"
                                "                      HDA Intel PCH at 0xf7e30000 irq 10\n"
                                " 1 [HDMI           ]: HDA-Intel - HDA Intel HDMI\n"
                                "                      HDA Intel HDMI at 0xf7e34000 irq 11\n";
    pci_device_t other = pch;
    char name[24];
    int eld = ASOUND_FILE(1, ASOUND_ELD);

    analog_card = &pch;
    check(asound_card_count() == 2 && strcmp(proc(ASOUND_CARDS), cards) == 0,
          "cards: PCH 0 and HDMI 1, in Linux's lines");
    check(strcmp(proc(ASOUND_FILE(0, ASOUND_ID)), "PCH\n") == 0 &&
              strcmp(proc(ASOUND_FILE(1, ASOUND_ID)), "HDMI\n") == 0,
          "cards: card0/id PCH, card1/id HDMI");
    check(asound_lookup(1, "eld#0.0", 7) == eld && asound_lookup(0, "eld#0.0", 7) == -1 &&
              asound_lookup(1, "eld#0.", 6) == -1 && asound_lookup(2, "id", 2) == -1,
          "cards: the ELD files are card1's");
    check(asound_name(eld, name, (int)sizeof(name)) == 0 && strcmp(name, "eld#0.0") == 0 &&
              asound_entry(1, 1) == eld && asound_entry(0, 1) == -1,
          "cards: card1 lists eld#0.0 after its id");
    check(asound_mode(eld) == 0644 && asound_mode(ASOUND_FILE(1, ASOUND_ID)) == 0444 &&
              asound_mode(ASOUND_CARDS) == 0444,
          "cards: an ELD file is 0644, the others 0444");
    check(strstr(proc(eld), "monitor_name\t\tIGT\n") != NULL, "cards: card1's eld#0.0 is IGT's");
    other.device_id = 0x293e;
    analog_card = &other;
    check(strncmp(proc(ASOUND_CARDS), " 0 [Intel          ]: HDA-Intel - HDA Intel\n", 44) == 0,
          "cards: ICH9's function is HDA Intel, id Intel");
    other.vendor_id = 0x1022;
    check(strncmp(proc(ASOUND_CARDS), " 0 [Generic        ]: HDA-Intel - HD-Audio Generic\n", 51) ==
              0,
          "cards: another vendor's is HD-Audio Generic");
    analog_card = &pch;
}

/** Root's write to an ELD file, as Linux's snd_hdmi_write_eld_info takes it. */
static void row_eld_write(void)
{
    static const char text[] = "monitor_name Foo\n"
                               "monitor_present 0\n"
                               "manufacture_id 0x1234\n"
                               "connection_type 7\n"
                               "port_id 0x123456789a\n"
                               "speakers 0x1\n"
                               "sad1_channels 0x5\n"
                               "sad20_rates 0x1\n"
                               "no value here\n"
                               "audio_sync_delay 0xffffffff";
    const struct hh_pin *p = &hh_pins[0];
    int eld = ASOUND_FILE(1, ASOUND_ELD);
    audio_stream_t s;

    check(asound_write(eld, text, (int)sizeof(text) - 1) == 0, "eld write: taken");
    check(p->monitor_present == 0 && p->eld_valid == 1 && strcmp(p->info.monitor_name, "IGT") == 0,
          "eld write: monitor_present set, the name kept");
    check(p->info.conn_type == 7 && p->info.port_id == 0x123456789aull && p->info.spk_alloc == 1 &&
              p->info.sad[1].channels == 5 && p->info.sad_count == 2 &&
              p->info.aud_synch_delay == -1,
          "eld write: the fields set, SAD 1 counted, SAD 20 passed over");
    check(strstr(proc(eld), "connection_type\t\treserved\n") != NULL &&
              strstr(proc(eld), "audio_sync_delay\t-1\n") != NULL &&
              strstr(proc(eld), "port_id\t\t\t0x123456789a\n") != NULL &&
              strstr(proc(eld), "manufacture_id\t\t0x1234") == NULL,
          "eld write: read back, a type past the table named reserved");
    check(asound_write(eld, "sad_count 0x30\nsad:_channels 3\n", 31) == 0 &&
              p->info.sad_count == 0x30 && p->info.sad[10].channels == 3 &&
              strstr(proc(eld), "sad15_coding_type") != NULL && strstr(proc(eld), "sad16_") == NULL,
          "eld write: a count past the table prints the 16 SADs there");
    check(asound_write(eld, "sad0_max_bitrate 1f400\n", 23) == 0 &&
              p->info.sad[0].max_bitrate == 128000 &&
              strstr(proc(eld), "sad0_max_bitrate\t128000\n") != NULL,
          "eld write: sad0_max_bitrate sets the bitrate the read prints");
    check(asound_write(eld, "sad_count 7fffffff\n", 19) == 0 && p->info.sad_count == 0x7fffffff,
          "eld write: a count of 7fffffff kept, as Linux keeps it");
    stream_init(&s, 48000, AUDIO_S16LE, 4096);
    check(hh_open0(&s) == 0, "eld write: an open limits by the 16 SADs there, no more");
    hh_stream_close(&s);
    check(asound_write(ASOUND_FILE(1, ASOUND_ID), "x 1\n", 4) == -EACCES &&
              asound_write(ASOUND_FILE(0, ASOUND_ELD), "x 1\n", 4) == -ENOENT,
          "eld write: only an ELD file of the HDMI card");
}

/** The analog card alone once the display goes. */
static void row_analog_alone(void)
{
    acomp_unregister_display(&disp_dev);
    check(asound_card_count() == 1 &&
              strcmp(proc(ASOUND_CARDS), " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n"
                                         "                      HDA Intel PCH at 0xf7e30000 irq "
                                         "10\n") == 0 &&
              asound_text(ASOUND_FILE(1, ASOUND_ID), NULL, 0) == -ENOENT,
          "analog alone: card 0 is listed, card1 gone");
    analog_card = NULL;
    check(strcmp(proc(ASOUND_CARDS), "--- no soundcards ---\n") == 0, "no card: none listed");
}

int main(void)
{
    row_d0();
    row_laps();
    row_convert();
    row_gone();
    row_cards();
    row_eld_write();
    row_analog_alone();
    if (failures == 0) {
        printf("hdmi-audio-linux-test: ok\n");
    }
    return failures != 0;
}
