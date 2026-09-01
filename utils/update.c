/**
 * update: the periodic sync, as V7's did.
 *
 * init starts one at boot on every image. It sleeps thirty seconds and
 * calls sync(2), for ever: file data written back through the buffer cache
 * reaches the disk within that time whether or not anything calls sync.
 * The kernel does not do this itself because its timer cannot sleep and it
 * has no thread of its own; a process is the right thing to charge.
 */
#include <unistd.h>

int main(void)
{
    for (;;) {
        sleep(30);
        sync();
    }
}
