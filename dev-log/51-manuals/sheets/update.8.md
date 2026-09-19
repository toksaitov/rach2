# update(8)

NAME: update - the periodic sync. Source: utils/update.c header, utils/Readme.md "Init and account programs".
* `update` takes no arguments. init starts one at boot on every image and keeps it running; it sleeps thirty seconds and calls sync(2), for ever, so data written through the buffer cache reaches the disk within that time whether or not anything calls sync. The kernel does not do this itself: its timer cannot sleep and it has no thread of its own.
* SEE ALSO: sync(2), init(8).
