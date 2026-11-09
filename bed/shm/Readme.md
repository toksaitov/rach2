# Shared-memory GPU transport

The test bed's transport carries virtio-gpu between Rach2's driver and a device backend in a Linux VM through one region of shared memory: a control area, the two split virtqueues and a heap from which the driver allocates every command, response and resource backing, with an offset into the region as the DMA address the device sees. The protocol on the queues is virtio-gpu, unchanged; only where the queues live and how the two sides meet are new. `vgpushm.h` fixes the layout and holds the checks both sides run, in C89 for Rach2's kernel and C99 for Linux; `peer.c` is a driver-side program for Linux that speaks the driver's half against the VM's backend or against a loopback device in the same file; `corpus/` holds the seed inputs of the fuzzers on both sides' parsers.

## Use

    cc -std=c99 -O2 -o build/bed/peer bed/shm/peer.c
    build/bed/peer -l [-r STREAM] [-d DIR] [-s SIZE]
    build/bed/peer -f PATH [-r STREAM] [-d DIR] [-t SECONDS]
    build/bed/peer -c IMAGE...
    build/bed/peer -w STREAM

* `-l` maps an anonymous region of `SIZE` bytes (32 KiB unless given), forks the loopback device and plays the driver against it: the handshake, `GET_DISPLAY_INFO`, then `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`, `TRANSFER_TO_HOST_2D`, `SET_SCANOUT` and a fenced `RESOURCE_FLUSH` of a patterned 32 by 32 resource, the stream named by `-r` if any, the teardown, and a reset into a second epoch in which an id of the first must be refused.
* `-f PATH` maps a file shared with QEMU and plays the same sequence, without the reset, against the backend in the VM, polling. QEMU maps the same file into the VM as an ivshmem device: `-object memory-backend-file,id=vgs,share=on,mem-path=/dev/shm/vgpu,size=32M -device ivshmem-plain,memdev=vgs`. A region already laid out keeps its epochs, so the device sees each run as a new connection.
* `-r STREAM` replays a recorded virgl stream in a context of its own; it needs the device to offer `VIRTIO_GPU_F_VIRGL`.
* `-d DIR` writes a copy of the region before each step the driver takes, as a seed.
* `-c` checks region images with the header's checks, as both sides would read them.
* `-w STREAM` writes the built-in stream, `streams/clear.rec`: a 32 by 32 render target made, a surface on it bound as the framebuffer and cleared to one colour.

## Results

The peer prints `peer: STEP: ok` for each command whose response is the one expected, `peer: flush: crc X matches` when the loopback device's scanout holds the bytes the driver wrote, and `peer: result: ok` at the end, and exits 0. The loopback device prints `responder: ...` lines for each epoch's offer, each submit's command count and any refusal. A wrong response, a fault in what the device wrote, the device's death or no answer within `-t` seconds (10 unless given) ends the peer with a message on stderr and status 1; usage errors exit 2. `-c` prints the rule each failing image breaks and `peer: check: N of M image(s) pass`, and exits 1 when one fails.

## Interfaces

All values are little-endian, as in virtio 1.x, and every structure has the same offsets on i686 and x86-64: each 64-bit field is 8-aligned by explicit padding, and the header asserts the sizes at compile time.

| Offset | Size | Contents | Writer |
|---|---|---|---|
| 0 | 64 | `struct vgs_layout`: magic `VGSH`, version 1.0, region size, ring area, heap | the driver, once, magic last |
| 256 | 112 | `struct vgs_driver`: epoch, state, peer id, error, accepted features, events seen, the two queues' setup | the driver |
| 1024 | 80 | `struct vgs_device`: epoch, state, peer id, error, offered features, limits, display events, scanouts, capsets | the device |
| 4096 | `ring_len` | the ring area: each queue's descriptor table, avail ring and used ring | the queues' usual writers |
| `heap_off` | `heap_len` | the heap: commands, responses and backing | the driver allocates; the device writes only writable descriptors |

The handshake, for each epoch:

1. The driver stores its block with a new epoch, never 0, and state `RESET`, after laying out a new region.
2. The device, seeing an epoch it has not answered, ends everything of the old one, checks the layout, writes its offer (features, limits, configuration), the epoch and state `READY`.
3. The driver checks the offer, writes the accepted features and the queues' setup, empties the ring area and stores state `OK`.
4. The device checks the setup against its own copy of the offer and stores `RUNNING`, or `FAILED` with the rule broken in its error field.

A device that meets a fault while running stores `NEEDS_RESET` with the error; the driver answers with a new epoch. Either side stores `GONE` when it detaches in order. Display events are a count, `events_seq`, which the driver's virtio-gpu code sees as `VIRTIO_GPU_EVENT_DISPLAY` while it differs from the `events_seen` the driver last stored.

Doorbells are a fixed pair of ivshmem vectors: 0 for "look at your queues", 1 for "look at the control area"; `VGS_DOORBELL(peer, vector)` is the value for ivshmem's doorbell register. They are used only when both sides accept `VGS_TF_DOORBELL`; without it both poll.

Ids: the driver makes each resource and context id from a 24-bit slot and an 8-bit generation, `VGS_ID_MAKE(slot, gen)`, the generation moving each time the slot is reused. The device treats ids as opaque.

A recorded stream is the bytes `VGSREC1\n` followed by records, each a 32-bit kind, a 32-bit length and that many bytes: kind 1 is a `RESOURCE_CREATE_3D` without its header (48 bytes), which the peer makes, backs from the heap and attaches to the context; kind 2 is a whole number of virgl dwords for one `SUBMIT_3D`.

## Design

These are the transport's rules; the header's functions enforce the ones a parser can check, and the device's command code the quotas.

* One writer for each word. The layout is the driver's, written before any state; the driver's block is the driver's and the device's block the device's. A block's writer changes its other fields only before storing a new epoch or state, with a barrier between; a reader copies the block between two reads of its epoch and state, a 32-bit word at a time, and keeps the copy only when they agree (`vgs_snap_driver`, `vgs_snap_device`). Each side checks and works from its copy, never from the region again, since the other side may change the region at any time.
* Every offset and length is checked. The layout puts the control area in the first page, the ring area after it and the heap after that, each page-aligned and nonempty in a region whose size is a power of two from 32 KiB to 64 GiB (`vgs_check_layout`). Each queue has a power-of-two size no larger than the device's `max_queue`, its parts aligned as virtio requires, inside the ring area and apart from each other and from the other queue's (`vgs_check_queue`, `vgs_check_accept`). Every buffer either side names, a descriptor's or a backing entry's, lies inside the heap, never in the control area or the rings, with the sum computed without overflow (`vgs_check_range`). A ring index may move by at most the queue's size (`vgs_avail_read`, `vgs_used_read`), and a used element names a descriptor of the queue (`vgs_used_elem`); the driver also checks that it names a chain outstanding and reports no more bytes than that chain could take.
* A chain is read once. `vgs_chain_read` copies each descriptor out of the table as it walks it, refuses indirect descriptors and any unknown flag, stops a loop at the device's `max_chain` or the queue's size, whichever is smaller, requires readable segments before writable ones and holds each way to `max_cmd` bytes. The device works only from the copy.
* Features: the driver accepts a subset of the offer within the transport's allowed set, virtio-gpu's own bits and `VIRTIO_F_VERSION_1`, which is required. Indirect descriptors, event indices and packed rings are refused by version 1.
* Lifetimes and epochs live in the transport's tables, never in virtio-gpu's commands. Each side keeps its own `struct vgs_table` in its own memory: every resource and context, the epoch it was made in and the backing charged to it. A command finds its objects only in the current epoch; a new epoch ends every object of the old one, on both sides, and the driver fails every fence still pending in it. A virtio-gpu command carries only the ids it always carries.
* Quotas: the device's limits bound live resources, live contexts, total backing bytes, entries in one `ATTACH_BACKING`, descriptors in one chain and bytes in one chain. The table refuses an object or a charge past them (`vgs_table_add`, `vgs_table_charge`), and the device answers with virtio-gpu's own `ERR_OUT_OF_MEMORY` or an invalid-id error; the driver's own allocations come from the heap, and a full heap is `ENOMEM`, never a fallback outside the region.
* A fault in the transport, as opposed to a refused command, stops the side that meets it: the device stores `NEEDS_RESET`, and the driver starts a new epoch or detaches.
* The other side's death ends the epoch: the driver learns of the VM's from the ivshmem server, or here from the loopback device's exit, and fails every wait.

## Adding tests

The seeds in `corpus/` are the images `peer -l -r streams/clear.rec -d corpus` writes, one before each step of the loopback run, 32 KiB each; the run is deterministic, so the same command makes the same files. `peer -c corpus/*` must pass every one. A fuzzer's minimised crash joins the corpus of the side whose parser it broke.

## Limits

The committed stream is written by `peer -w` from virgl's protocol header, not captured from Mesa; a stream recorded at the virtio-gpu level replays the same way. The loopback device checks a submit's framing, not its meaning: it renders nothing in 3D. The peer keeps one command outstanding at a time and polls; doorbells come with Rach2's daemon.
