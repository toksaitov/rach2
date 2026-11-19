/*
 * dsp_gone_hosttest.c: /dev/dsp (audio/dsp.c) over a device of one
 * direction and over one that goes while an open waits on it, on the model
 * driver of dsp_hosttest.c.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/audio/resample.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The rows: O_RDWR on a device without capture opens playback alone, and
 * on one without playback capture alone, as Linux's snd_pcm_oss_open_file
 * opens the directions a PCM has, and on one with neither fails; a device
 * that goes, its driver calling the stream's `gone`, wakes a write blocked
 * on a full buffer, which returns the bytes it queued, and a drain, which
 * fails, as Linux's snd_card_disconnect wakes them; after it every call
 * fails with ENODEV, poll reports POLLERR and POLLNVAL, and the close
 * neither waits nor asks the driver to play.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#define main dsp_rows
#include "dsp_hosttest.c"
#undef main

/** The open on `entry`. */
static struct dsp *open_of(void)
{
    return (struct dsp *)entry.priv;
}

/** A wait's hook: the device goes, its driver telling the open, once. The
 * model plays on, so a wait that `gone` did not end would end when the
 * model made room, and the rows would see the whole write or the drain's
 * success. */
static void device_goes(void)
{
    wait_hook = NULL;
    open_of()->play.stream.gone(&open_of()->play.stream);
}

/** O_RDWR on a device of one direction opens that direction alone. */
static void row_one_direction(void)
{
    begin("O_RDWR without capture");
    cap.refuse_open = -ENODEV;
    check(open_dsp(2) == 0, "O_RDWR without capture succeeds");
    check(m.s != NULL && cap.s == NULL && open_of()->rec.ring == NULL, "playback alone is taken");
    check(ask(DSP_GETCAPS, 0) == (DSP_CAP_REVISION | DSP_CAP_REALTIME | DSP_CAP_TRIGGER),
          "no duplex among the caps");
    check(ask(DSP_GETTRIGGER, 0) == PCM_ENABLE_OUTPUT, "the output trigger alone");
    check(dspdev_read(&entry, (char __user *)got, 4) == -EBADF, "a read is EBADF");
    check(put(0, 512) == 512 && m.running, "a write plays");
    close_dsp();
    check(m.closes == 1 && cap.closes == 0, "playback's stream given back");

    begin("O_RDWR without playback");
    m.refuse_open = -ENODEV;
    check(open_dsp(2) == 0, "O_RDWR without playback succeeds");
    check(m.s == NULL && cap.s != NULL && open_of()->play.ring == NULL, "capture alone is taken");
    check(put(0, 4) == -EBADF, "a write is EBADF");
    close_dsp();

    begin("O_RDWR on neither");
    m.refuse_open = -ENODEV;
    cap.refuse_open = -ENODEV;
    check(open_dsp(2) == -ENODEV && m.s == NULL && cap.s == NULL,
          "O_RDWR on a device of neither direction is ENODEV");
}

/** A device that goes wakes a blocked write and a drain, and every call on
 * the open fails after it. */
static void row_gone(void)
{
    struct wait_queue *wq;
    int value = 0;
    int32_t n;

    begin("gone during a write");
    check(open_dsp(1) == 0, "an open for writing succeeds");
    wait_hook = device_goes;
    n = put(0, 4096);
    check(n > 0 && n < 4096, "a write blocked on a full buffer returns what it queued");
    check(open_of()->play.gone && wait_hook == NULL, "the device went during its wait");
    check(put(0, 4) == -ENODEV, "the next write is ENODEV");
    check(call(DSP_GETOSPACE, &value) == -ENODEV, "an ioctl is ENODEV");
    check(dspdev_poll(&entry, POLLOUT, &wq) == (POLLERR | POLLNVAL), "poll: POLLERR and POLLNVAL");
    close_dsp();
    check(m.closes == 1, "the close gives the stream back without waiting");

    begin("gone during a drain");
    check(open_dsp(1) == 0, "an open for writing succeeds");
    check(put(0, 512) == 512 && m.running, "a write plays");
    wait_hook = device_goes;
    check(call(DSP_SYNC, &value) == -ENODEV, "SYNC ends with the device, ENODEV");
    check(!open_of()->play.running, "playback is no longer counted as running");
    close_dsp();
}

/** Run the rows. */
int main(void)
{
    uint32_t i;

    for (i = 0; i < PATTERN; i++) {
        pattern[i] = (uint8_t)(i * 7 + 1 + (i >> 8));
    }
    row_one_direction();
    row_gone();
    if (failures) {
        printf("dsp-gone-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("dsp-gone-hosttest: ok\n");
    return 0;
}
