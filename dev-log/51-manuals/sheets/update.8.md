# update(8)

NAME: update - the periodic sync. Source: utils/update.c header, utils/Readme.md "Init and account programs".
* `update` takes no arguments. init starts one at boot on every image and keeps it running; it sleeps thirty seconds and calls sync(2), for ever, so data written through the buffer cache is written out about every thirty seconds whether or not anything calls sync; it promises no deadline. The kernel does not do this itself: its timer cannot sleep and it has no thread of its own.
* SEE ALSO: sync(2), init(8).
* Found at the proofreading: the test image has no init, and the sealed root's manifest did not stage update at all until this milestone (entry 109), so init there forked a failing exec every second; init starts it again a second after it exits.
