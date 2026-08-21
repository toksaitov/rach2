/*
 * audio_debug.c - Kernel shell audio debug helpers
 */

#include "audio_debug.h"
#include "hda.h"
#include "../kio.h"
#include "../lib/kstring.h"

extern volatile uint32_t kernel_ticks;

/*
 * State for the kernel-side square-wave test tone. The mixer callback runs in
 * interrupt context and reads these while the shell command writes them, so
 * they are volatile. _tone_half_period is the count of frames in each half of
 * the wave; _tone_phase counts frames within one full period.
 */
static volatile uint32_t _tone_phase = 0;
static volatile uint32_t _tone_half_period = 0;
static volatile int16_t _tone_amp = 8000;
static volatile int _tone_channels = 2;

static bool audio_str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == *b;
}

static bool audio_str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) {
            return false;
        }
        ++str;
        ++prefix;
    }

    return true;
}

/**
 * Split `buffer` into space- or tab-delimited tokens in place.
 *
 * Each separator run is overwritten with a NUL and the first character of each
 * token is recorded in `argv`. At most `max_args` tokens are stored; any tail
 * beyond that is left untouched in the buffer.
 *
 * @return the number of tokens stored in `argv`.
 */
static int audio_tokenize(char *buffer, char **argv, int max_args)
{
    int argc = 0;
    char *p;
    int in_arg = 0;

    for (p = buffer; *p != '\0' && argc < max_args; ++p) {
        if (*p == ' ' || *p == '\t') {
            *p = '\0';
            in_arg = 0;
        } else if (!in_arg) {
            argv[argc++] = p;
            in_arg = 1;
        }
    }

    return argc;
}

/**
 * Parse a non-negative decimal integer with no sign or whitespace.
 *
 * The whole string must be digits; anything else (including an empty string or
 * a null `str` or `out`) fails and leaves `*out` unchanged. Overflow past 32
 * bits wraps instead of being rejected.
 *
 * @return true on success, false on a malformed input.
 */
static bool audio_parse_uint(const char *str, uint32_t *out)
{
    uint32_t value = 0;

    if (!str || *str == '\0' || !out) {
        return false;
    }

    while (*str) {
        char c = *str++;
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + (uint32_t)(c - '0');
    }

    *out = value;
    return true;
}

/**
 * Mixer callback that fills `buffer` with the current square-wave tone.
 *
 * Runs in interrupt context from the driver's playback path. It emits signed
 * 16-bit samples driven by the file-scope _tone_* state, duplicating each
 * sample across both channels when stereo. `size` is in bytes, treated as
 * `size / 2` samples (a trailing odd byte is dropped). In stereo, the
 * `i + 1 < samples16` guard writes a final lone sample to one channel only, so
 * the buffer is never overrun.
 */
static void audio_test_callback(uint8_t *buffer, size_t size)
{
    size_t i;
    int16_t *out = (int16_t *)buffer;
    size_t samples16 = size / 2;

    for (i = 0; i < samples16; ) {
        int16_t s = (_tone_phase < _tone_half_period) ? _tone_amp : (int16_t)(-_tone_amp);
        _tone_phase++;
        if (_tone_phase >= (_tone_half_period * 2u)) {
            _tone_phase = 0;
        }

        if (_tone_channels == 2) {
            if (i + 1 < samples16) {
                out[i++] = s;
                out[i++] = s;
            } else {
                out[i++] = s;
            }
        } else {
            out[i++] = s;
        }
    }
}

/**
 * Stop any active playback and detach the test callback.
 *
 * Safe to call when no driver is bound or nothing is playing.
 */
static void audio_stop_playback(void)
{
    if (!audio) {
        return;
    }

    if (audio->is_playing && audio->is_playing()) {
        audio->stop();
    }

    if (audio->set_callback) {
        audio->set_callback((audio_callback_t)0);
    }
}

/**
 * Spin until `kernel_ticks` reaches `target_ticks`, halting between ticks.
 *
 * The `hlt` parks the CPU until the next interrupt (the timer tick advances
 * kernel_ticks), so the wait does not busy-burn the core.
 */
static void audio_wait_until(uint32_t target_ticks)
{
    while (kernel_ticks < target_ticks) {
        __asm__ __volatile__("hlt");
    }
}

/**
 * Play a five-note C-major scale through the callback path.
 *
 * Drives the driver in 48 kHz stereo signed-16 mode, retunes the shared tone
 * state once per note, and lets the interrupt-time callback synthesize the
 * samples. The notes split `duration_ms` evenly; the total is clamped to at
 * least 100 ms. Always stops playback on the way out.
 */
static void audio_run_scale_test(uint32_t duration_ms)
{
    uint32_t start_ticks;
    uint32_t total_ticks;
    uint32_t sample_rate = 48000;
    int channels = 2;
    size_t n;

    static const uint32_t freqs[] = { 262, 294, 330, 349, 392 };
    const size_t num_notes = sizeof(freqs) / sizeof(freqs[0]);

    if (!audio) {
        k_puts("No audio driver.");
        return;
    }

    if (duration_ms < 100) {
        duration_ms = 100;
    }

    audio_stop_playback();

    _tone_channels = channels;
    _tone_amp = 12000;

    {
        uint32_t period = sample_rate / freqs[0];
        if (period < 2) {
            period = 2;
        }
        _tone_half_period = period / 2;
        if (_tone_half_period == 0) {
            _tone_half_period = 1;
        }
        _tone_phase = 0;
    }

    if (audio->set_callback) {
        audio->set_callback(audio_test_callback);
    }

    if (audio->start(sample_rate, AUDIO_S16LE, channels) != 0) {
        k_puts("AUDIO_START failed.");
        audio_stop_playback();
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    k_printf("Playing scale test (%d ms) using %s...\n", duration_ms, audio_get_driver_name());

    start_ticks = kernel_ticks;
    total_ticks = duration_ms / 10;
    if (total_ticks == 0) {
        total_ticks = 1;
    }

    for (n = 0; n < num_notes; ++n) {
        uint32_t period = sample_rate / freqs[n];
        uint32_t note_end_ticks;

        if (period < 2) {
            period = 2;
        }

        _tone_half_period = period / 2;
        if (_tone_half_period == 0) {
            _tone_half_period = 1;
        }
        _tone_phase = 0;

        note_end_ticks = start_ticks + (uint32_t)(((n + 1u) * total_ticks) / num_notes);
        audio_wait_until(note_end_ticks);
    }

    audio_stop_playback();
    k_puts("Done.");
}

/**
 * Play the same scale through the blocking write path instead of the callback.
 *
 * Uses 11025 Hz unsigned-8 mono and synthesizes samples in this thread,
 * pushing them with audio->write(). Each timer tick it computes how many frames
 * are due (sample_acc keeps the fractional remainder across ticks so the rate
 * stays exact over time) and writes them in <= sizeof(buf) chunks, halting on a
 * short write until the driver drains. Exercises the write side that the
 * callback test bypasses.
 */
static void audio_run_write_test(uint32_t duration_ms)
{
    uint32_t start_ticks;
    uint32_t total_ticks;
    uint32_t sample_rate = 11025;
    uint32_t tick_last;
    uint32_t sample_acc = 0;
    uint32_t phase = 0;
    uint32_t half_period = 0;
    size_t n;
    uint8_t buf[512];
    const uint8_t amp = 64;

    static const uint32_t freqs[] = { 262, 294, 330, 349, 392 };
    const size_t num_notes = sizeof(freqs) / sizeof(freqs[0]);

    if (!audio) {
        k_puts("No audio driver.");
        return;
    }

    if (duration_ms < 100) {
        duration_ms = 100;
    }

    audio_stop_playback();

    if (audio->start(sample_rate, AUDIO_U8, 1) != 0) {
        k_puts("AUDIO_START failed.");
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    k_printf("Playing write-based scale test (%d ms) using %s...\n", duration_ms, audio_get_driver_name());

    start_ticks = kernel_ticks;
    total_ticks = duration_ms / 10;
    if (total_ticks == 0) {
        total_ticks = 1;
    }

    tick_last = kernel_ticks;

    for (n = 0; n < num_notes; ++n) {
        uint32_t period = sample_rate / freqs[n];
        uint32_t note_end_ticks;

        if (period < 2) {
            period = 2;
        }

        half_period = period / 2;
        if (half_period == 0) {
            half_period = 1;
        }
        phase = 0;

        note_end_ticks = start_ticks + (uint32_t)(((n + 1u) * total_ticks) / num_notes);
        while (kernel_ticks < note_end_ticks) {
            uint32_t to_write;

            while (kernel_ticks == tick_last) {
                __asm__ __volatile__("hlt");
            }
            tick_last = kernel_ticks;

            sample_acc += sample_rate;
            to_write = sample_acc / 100u;
            sample_acc %= 100u;

            while (to_write > 0) {
                uint32_t chunk = (to_write > (uint32_t)sizeof(buf)) ? (uint32_t)sizeof(buf) : to_write;
                uint32_t i;
                uint32_t off = 0;

                for (i = 0; i < chunk; ++i) {
                    uint8_t v = (phase < half_period) ? (uint8_t)(128 + amp) : (uint8_t)(128 - amp);
                    buf[i] = v;
                    phase++;
                    if (phase >= half_period * 2u) {
                        phase = 0;
                    }
                }

                while (off < chunk) {
                    int written = audio->write(buf + off, (size_t)(chunk - off));

                    if (written < 0) {
                        k_puts("AUDIO_WRITE failed.");
                        audio_stop_playback();
                        return;
                    }
                    if (written == 0) {
                        __asm__ __volatile__("hlt");
                        continue;
                    }

                    off += (uint32_t)written;
                }

                to_write -= chunk;
            }
        }
    }

    audio_stop_playback();
    k_puts("Done.");
}

/**
 * Play a steady tone while periodically dumping the HDA stream position.
 *
 * Only valid when the active driver is HDA. It plays `freq_hz` and, every
 * `interval_ms`, reads the link position-in-buffer (LPIB) and stream registers
 * via the debug snapshot, printing the LPIB delta since the last sample. The
 * delta wraps modulo the cyclic buffer length (CBL), so a DMA that has looped
 * past the buffer end still reports forward progress. Useful for confirming the
 * stream's DMA engine is actually advancing.
 */
static void audio_run_hda_watch(uint32_t freq_hz, uint32_t duration_ms, uint32_t interval_ms)
{
    uint32_t start_ticks;
    uint32_t end_ticks;
    uint32_t next_ticks;
    uint32_t sample_rate = 48000;
    uint32_t last_lpib = 0;
    uint32_t period;
    hda_stream_debug_snapshot_t snapshot;
    int channels = 2;

    if (!audio) {
        k_puts("No audio driver.");
        return;
    }
    if (audio->type != AUDIO_DRIVER_HDA) {
        k_puts("HDA not active.");
        return;
    }
    if (freq_hz == 0) {
        freq_hz = 440;
    }
    if (interval_ms == 0) {
        interval_ms = 100;
    }

    audio_stop_playback();

    period = sample_rate / freq_hz;
    if (period < 2) {
        period = 2;
    }

    _tone_channels = channels;
    _tone_half_period = period / 2;
    if (_tone_half_period == 0) {
        _tone_half_period = 1;
    }
    _tone_phase = 0;
    _tone_amp = 12000;

    if (audio->set_callback) {
        audio->set_callback(audio_test_callback);
    }

    if (audio->start(sample_rate, AUDIO_S16LE, channels) != 0) {
        k_puts("AUDIO_START failed.");
        audio_stop_playback();
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    if (!hda_debug_get_stream_snapshot(&snapshot)) {
        k_puts("HDA snapshot unavailable.");
        audio_stop_playback();
        return;
    }

    last_lpib = snapshot.lpib;

    k_printf("HDA watch: %d Hz, %d ms, interval %d ms\n", freq_hz, duration_ms, interval_ms);

    start_ticks = kernel_ticks;
    end_ticks = start_ticks + (duration_ms / 10u);
    next_ticks = start_ticks;

    while (kernel_ticks < end_ticks) {
        if (kernel_ticks >= next_ticks) {
            if (hda_debug_get_stream_snapshot(&snapshot)) {
                if (snapshot.cbl != 0) {
                    uint32_t delta = (snapshot.lpib >= last_lpib)
                                   ? (snapshot.lpib - last_lpib)
                                   : (snapshot.lpib + snapshot.cbl - last_lpib);
                    k_printf("t=%dms lpib=%d (+%d) ctl=0x%x sts=0x%x fmt=0x%x\n",
                             (kernel_ticks - start_ticks) * 10u,
                             snapshot.lpib,
                             delta,
                             snapshot.ctl,
                             (uint32_t)snapshot.sts,
                             (uint32_t)snapshot.fmt);
                } else {
                    k_printf("t=%dms lpib=%d ctl=0x%x sts=0x%x fmt=0x%x\n",
                             (kernel_ticks - start_ticks) * 10u,
                             snapshot.lpib,
                             snapshot.ctl,
                             (uint32_t)snapshot.sts,
                             (uint32_t)snapshot.fmt);
                }
                last_lpib = snapshot.lpib;
            }

            next_ticks += (interval_ms / 10u);
            if (interval_ms < 10) {
                next_ticks += 1;
            }
        }

        __asm__ __volatile__("hlt");
    }

    audio_stop_playback();
    k_puts("Done.");
}

void audio_debug_print_help(void)
{
    k_puts("Audio commands:");
    k_puts("  audio info");
    k_puts("  audio test scale [duration_ms]");
    k_puts("  audio test write [duration_ms]");
    k_puts("  audio test watch [freq_hz] [duration_ms] [interval_ms]");
    k_puts("  audio hda dump");
    k_puts("  audio hda codec");
    k_puts("  audio hda pins");
    k_puts("  audio hda stats");
    k_puts("  audio hda out [auto|primary|hp|both]");
}

static void audio_print_summary(void)
{
    k_printf("Audio driver: %s\n", audio_get_driver_name());
    k_printf("Real output:  %s\n", audio_has_real_output() ? "yes" : "no");
    k_puts("Use 'audio help' for commands.");
}

/**
 * Handle an `audio ...` shell command line.
 *
 * Recognizes a line whose first token is exactly "audio" (so it does not claim
 * "audioctl" or the like), copies it into a local buffer, tokenizes in place,
 * and dispatches the subcommand. A bare "audio" or an unknown form prints the
 * summary or help text.
 *
 * @return true if the line was an audio command (handled here), false if it
 *         belongs to another handler.
 */
bool audio_debug_handle_command(const char *cmd)
{
    char buffer[128];
    char *argv[8];
    int argc;
    if (!cmd || !audio_str_starts_with(cmd, "audio")) {
        return false;
    }
    if (cmd[5] != '\0' && cmd[5] != ' ' && cmd[5] != '\t') {
        return false;
    }

    k_strncpy(buffer, cmd, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    argc = audio_tokenize(buffer, argv, 8);

    if (argc <= 1) {
        audio_print_summary();
        return true;
    }

    if (audio_str_equal(argv[1], "help")) {
        audio_debug_print_help();
        return true;
    }

    if (audio_str_equal(argv[1], "info")) {
        audio_print_summary();
        return true;
    }

    if (audio_str_equal(argv[1], "test")) {
        uint32_t duration_ms = 2000;

        if (argc < 3) {
            audio_debug_print_help();
            return true;
        }

        if (audio_str_equal(argv[2], "scale")) {
            if (argc >= 4 && !audio_parse_uint(argv[3], &duration_ms)) {
                k_puts("Usage: audio test scale [duration_ms]");
                return true;
            }
            audio_run_scale_test(duration_ms);
            return true;
        }

        if (audio_str_equal(argv[2], "write")) {
            if (argc >= 4 && !audio_parse_uint(argv[3], &duration_ms)) {
                k_puts("Usage: audio test write [duration_ms]");
                return true;
            }
            audio_run_write_test(duration_ms);
            return true;
        }

        if (audio_str_equal(argv[2], "watch")) {
            uint32_t freq_hz = 440;
            uint32_t interval_ms = 100;

            if (argc >= 4 && !audio_parse_uint(argv[3], &freq_hz)) {
                k_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }
            if (argc >= 5 && !audio_parse_uint(argv[4], &duration_ms)) {
                k_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }
            if (argc >= 6 && !audio_parse_uint(argv[5], &interval_ms)) {
                k_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }

            audio_run_hda_watch(freq_hz, duration_ms, interval_ms);
            return true;
        }

        audio_debug_print_help();
        return true;
    }

    if (audio_str_equal(argv[1], "hda")) {
        if (argc < 3) {
            audio_debug_print_help();
            return true;
        }

        if (audio_str_equal(argv[2], "dump")) {
            hda_debug_dump();
            return true;
        }
        if (audio_str_equal(argv[2], "codec")) {
            hda_debug_dump_codec();
            return true;
        }
        if (audio_str_equal(argv[2], "pins")) {
            hda_debug_dump_pins();
            return true;
        }
        if (audio_str_equal(argv[2], "stats")) {
            hda_debug_dump_stats();
            return true;
        }
        if (audio_str_equal(argv[2], "out")) {
            if (argc < 4) {
                k_printf("HDA out mode=%d\n", hda_get_output_mode());
                k_puts("Usage: audio hda out [auto|primary|hp|both]");
                return true;
            }

            if (audio_str_equal(argv[3], "auto")) {
                hda_set_output_mode(0);
            } else if (audio_str_equal(argv[3], "primary")) {
                hda_set_output_mode(1);
            } else if (audio_str_equal(argv[3], "hp")) {
                hda_set_output_mode(2);
            } else if (audio_str_equal(argv[3], "both")) {
                hda_set_output_mode(3);
            } else {
                k_puts("Usage: audio hda out [auto|primary|hp|both]");
                return true;
            }

            hda_debug_dump_pins();
            return true;
        }

        audio_debug_print_help();
        return true;
    }

    audio_debug_print_help();
    return true;
}
