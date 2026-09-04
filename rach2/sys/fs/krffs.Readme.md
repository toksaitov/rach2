# KRFFS in the kernel

KRFFS is this project's own file system and the root a Rach2 machine boots. `krffs.c` has the format: the node ring, the allocator, the operations the VFS adapter calls, and the check `init_krffs` makes before the root is used. The host tools under [`krffs/`](../../../krffs/Readme.md) build, read and repair the same images, and `fsck.krffs` is the independent judge of everything the kernel writes.

## The format

An image is a ring of nodes covering every byte of the region, with no gaps and no free list beside the ring itself. A node is a packed header (`krffs_node_t`, 357 bytes, the on-disk ABI) followed by its data. The header has a magic number, a type, an identifier, a name, a mode with the owner and the group, three times, the node's whole size in bytes including the header, the size of the node before it, the bytes of data in use, and the identifier of the directory it belongs to.

The first node is the root, which is also the root directory. The rest are reserved, holding a file or a directory, or free. Walking forward means adding a node's size to its address; the walk returns to the root, which is what makes it a ring. Walking backward means subtracting the size the node records for the one before it. Allocation is first fit with split and coalesce, after the storage allocator in The C Programming Language.

A name is one component, not a path, and a file is found by its directory and its name: `parent_id` says which directory holds it, and the root directory's identifier is zero. Two files in different directories may have the same name; two in one directory may not. A directory's own identifier is a hash of its parent's identifier and its name, and its children carry it in their `parent_id`, which is why a directory is never renamed or moved: its children reference the identifier the old name produced. A regular file's identifier is not used to find it, so nothing depends on the name it was hashed from.

The root's mode has one bit the format reserves, `KRFFS_MODE_DIRTY`: the kernel sets it before the first change of a writable mount and clears it when a clean shutdown has flushed everything. An image found with the bit set says a check is owed, not what is wrong with it.

## What a mount requires

Before an image is used the whole ring is walked, and it is refused unless every node has the magic number and a known type, unless every node's size is at least a header and stays inside the region, unless every node's data fits between its header and its end, unless every reserved node's name ends within its field, unless every node's backward link is the size of the node before it and the root's is the size of the last, and unless the walk returns to the root.

The backward link is the one derived thing in that list: the forward chain already says what it must be. The kernel checks it and refuses an image that fails it, because a disagreement means something wrote the image without keeping the format's rules and the kernel has no repair pass of its own. `fsck.krffs` on the host rebuilds it from the forward chain, which is what makes it repairable rather than fatal.

## The order of writes

A write to an image on a disk reaches it a cache block at a time, and a machine can stop between any two of them. The rule is that the forward chain is the truth, and every field that reaches the medium leaves a ring that can be walked. Three consequences follow, and they are the order every operation in `krffs.c` keeps.

The bytes come before the field that publishes them. A node's data is written before the `data_size` that says how much of it counts, and a new node's header is written into the free space it will occupy before the size that reveals it. A machine that stops between the two leaves a ring that is intact and a file that is the length it was.

One field publishes one change. A split writes the tail's free header inside the free node, where nothing yet reaches it, and then stores the shortened size, which is the moment the ring gains a node. Freeing a node stores its type, and then adds its size to the free node before it, which is the moment the ring loses one. Nothing is published by two fields a stop could separate. An operation may publish twice, so long as each store leaves a ring that can be walked: growing a file does, because the header of the node it grows into has to go before the tail's header can be written anywhere safe.

The backward link comes last and may lag. Every operation that changes a size repairs the following node's backward link afterwards, and a machine that stops before that leaves the one thing the check can rebuild from the ring it has just walked.

The operations in those terms:

* Create. Split the free node, write the header's fields into it, then store the type, which publishes the node, and write the name after that. Between the two the node is reserved with no name, which is a node no path reaches; the check reclaims one left there by a stop.
* Write past the end. Take the whole free node after it with one size, and split the surplus back off with a second: the tail's header is written into space the node already covers, and the size that shortens the node is what reveals it. Then write the caller's bytes, then store `data_size`. Writing that header before the free node is taken would land on the free node's own header whenever the growth is shorter than a header, and leave a torn node in the ring. The growth changes what the node can hold; the length changes only when the bytes are there.
* Truncate. Store the shorter `data_size` first, since the bytes beyond it stay inside the node, then write the surplus's free header and store the shortened size. A surplus too small to hold a header of its own stays in the node, where a later shrink or a removal reclaims it.
* Remove. Store the type as free, then merge with a free successor by one size, then with a free predecessor by one size.
* Move. A file that grows with no room after it is copied into a node taken elsewhere and the old node is then freed. The copy is published under the file's name before the old node goes, so a stop between the two leaves one name on two nodes. The check resolves it: a move only ever makes a node larger, so the larger of the two is the copy the move was making, and the smaller loses its name.
* Rename. The name, the identifier and the parent are three fields, and a stop can leave a name half written. The file stays reachable under whatever name the medium has; nothing else is damaged, and the check has nothing to repair.

## What a stop can leave, and what the check does

A ring whose backward links disagree with the forward chain. The check rebuilds them from the walk it has just made, and says how many.

A reserved node with no name, which is a node published before it was named, or one unlinked while a program still had it open. No path reaches it. The check frees it and merges it with its free neighbours, and says how many.

One name on two nodes in one directory, which is a move that was interrupted. The check keeps the larger node and takes the name off the smaller, which makes it the previous case.

The dirty flag of an image that was not cleanly unmounted, which says only that a check is owed. `fsck.krffs` clears it once the three above are settled.

Nothing else. A node whose header is torn, whose size leaves the region, whose data does not fit, or whose name never ends is not a state this order of writes can produce, so the check refuses the image and changes nothing rather than guessing what it should have been. `fsck.krffs` on the host makes the same judgements and repairs the four things above; `--strict` there reports them and changes nothing, which is how a test asks whether the kernel left an image clean rather than repairable.
