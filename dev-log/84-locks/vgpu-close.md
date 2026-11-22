# Task 5697: virtio-gpu's DETACH at GEM close and prompt frees

Task 5676 measured piglit's streaming-texture-leak through Linux 6.12's virtio-gpu and through Rach2's under the same QEMU and virgl. Rach2's RESOURCE_UNREFs after each submission reached the host over 0.74 to 0.79 s, against Linux's 0.03 to 0.04 s, and Rach2 made 10,001 RESOURCE_CREATEs against Linux's 6,266. This record measures where Rach2's time goes before any change, and what the change does to it.

## Setup

The testing PC under WSL, one guest, KVM, 8 CPUs and 8 GiB, the `virgl` display profile, with task 5676's tools from `dev-log/86-shared-region/measure/` on `task/5676`: `rach2.py` boots `gnu-test.disk` of `milestone/76-display` with a kernel of the same head, runs `stl.sh` as the test account and traces QEMU's resource and submission events. The kernel carries task 5676's counters and the ones in [`vgpu-close/counters.patch`](vgpu-close/counters.patch), which `/proc/stat` shows as `vgpu_m_*`:

* `vgpu_m_sub_worker` and `vgpu_m_sub_other`: submissions whose answer the worker reaped, and those another thread reaped; `vgpu_m_sub_lag_ns` and `_max_ns`, the time from the answer's interrupt to the reap of the submission; `vgpu_m_sub_done_ns`, the time in its completion, which drops the submission's references to its buffers.
* `vgpu_m_free_worker` and `vgpu_m_free_other`: objects freed (`vgpu_obj_free`) in the worker and elsewhere; `vgpu_m_free_ns`, the time in those frees; `vgpu_m_detach_ns`, the part of it in `vgpu_resource_detach`; `vgpu_m_release_ns`, the time in `obj_release` after the UNREF's answer.
* `vgpu_m_reap_notifies` and `_ns`: notifies made inside a reap, and their time.

[`vgpu-close/bursts.py`](vgpu-close/bursts.py) reads QEMU's trace and gives, for each submission, the UNREFs up to the next one, grouped into bursts whose UNREFs come less than 50 ms apart.

    make ARCH=x86_64 rach2 build/x86_64/images/gnu-test.disk
    python3 dev-log/86-shared-region/measure/rach2.py build/runs/task-5697/before-1
    python3 dev-log/84-locks/vgpu-close/bursts.py build/runs/task-5697/before-1/trace

## Before the change

The run that ended at 14:55 on 2026-11-22 (UTC+6): pass, in 17.1 s, with 10,001 creates, 10,001 UNREFs, a peak of 2,525 resources and 1,291,760 pages, as task 5676 found.

| Counter | Value |
|---|---|
| Submissions reaped by the worker, by another thread | 6, 0 |
| Answer to reap, in all and the longest | 9.7 ms, 8.7 ms |
| Time in the submissions' completions | 0.479 s |
| Objects freed in the worker, elsewhere | 4,997, 5,004 |
| Time in the frees, of it in `vgpu_resource_detach` | 2.437 s, 1.746 s |
| Time in `obj_release` after the UNREFs' answers | 0.262 s |
| Notifies, their time, the longest | 50,018, 8.79 s, 1.163 s |
| Notifies inside a reap, their time | 4,487, 0.617 s |

The UNREFs after each submission, from QEMU's trace:

| Submission | At s | UNREFs | First, after the submission | Burst | Burst's length |
|---|---|---|---|---|---|
| 1 | 0.000 | 2,504 | 1,126.6 ms | 2,504 | 659.2 ms |
| 2 | 3.796 | 2,520 | 1,167.3 ms | 2,520 | 673.7 ms |
| 3 | 7.549 | 2,519 | 1,150.6 ms | 2,519 | 776.6 ms |
| 4 | 11.304 | 1,226 | 1,113.5 ms | 1,226 | 317.5 ms |
| 5 | 12.751 | 1 | 334.5 ms | 1 | 0 |
| 6 | 13.086 | 1,231 | 4.4 ms | 1,231 | 368.5 ms |

The test's 10,001 GEM_CLOSEs took 2.068 s in all (`ioctllog.py`).

## Where the time goes

The worker reaps every submission's answer, within 9 ms of its interrupt. The test's own ioctls reap none. So the late UNREFs are not a late reap. They come from the cost of each free once the reap has begun:

* A free walked every context's whole list of attachments (`vgpu_resource_detach`), some 2,500 records here, to find the object's, and then sent its CTX_DETACH_RESOURCE.
* Each free then sent two commands, the DETACH and the UNREF, each with its own notify. QEMU takes virtio-gpu's notifies without an ioeventfd, so each stops the processor while QEMU runs the queue: about 0.12 ms here under WSL's KVM.

A free took 0.244 ms on average, 0.175 ms of it in the detach. A submission's 2,500 frees, done one after the other in the worker, spread its UNREFs over 0.66 to 0.78 s, at 0.26 to 0.31 ms each.

The other 5,004 frees ran in the test's own GEM_CLOSEs: objects no submission still held, whose handle was their last reference. They cost the test the same two notifies each, 2.07 s of GEM_CLOSE in all.

## The change

* A file's GEM_CLOSE of its last handle to a buffer sends CTX_DETACH_RESOURCE for the file's context at once, as Linux's `virtio_gpu_gem_object_close` does, through a new `close` hook of `struct drm_gem_object_funcs`. The DETACH holds a reference to the buffer until its answer. Each buffer keeps the list of its attachments, so the free no longer walks every context's list.
* An answer that drops a buffer's last reference (a submission's, a transfer's, a DETACH's) hands the buffer to the GPU worker, which frees the answered buffers together and sends their UNREFs with one notify at the end of the pass. Mesa's first GEM_CLOSE after a flush waits for the host in its DETACH, so that the buffers Mesa's cache keeps are idle while they are still young enough to reuse; the cache drops a buffer older than 1 s.

## After the change

The run that ended at 15:04 on 2026-11-22 (UTC+6), the same setup with the change and the counters of [`vgpu-close/counters-after.patch`](vgpu-close/counters-after.patch), in which `vgpu_m_detach_ns` is the time in the new close hook: pass, in 11.9 s against 17.1 s.

    python3 dev-log/86-shared-region/measure/rach2.py build/runs/task-5697/after-1
    python3 dev-log/84-locks/vgpu-close/bursts.py build/runs/task-5697/after-1/trace
    python3 dev-log/86-shared-region/measure/ioctllog.py build/runs/task-5697/after-1/stl-out/calls

| Counter | Before | After | Linux 6.12, task 5676's |
|---|---|---|---|
| RESOURCE_CREATEs and UNREFs | 10,001 | 6,269 | 6,266 |
| WAITs, of them NOWAIT, of them busy | 4, 3, 0 | 3,736, 3,735, 0 | NOWAIT 3,736 to 3,737 |
| Peak resources, pages | 2,525, 1,291,760 | 2,524, 1,291,759 | |
| Objects freed in the worker, elsewhere | 4,997, 5,004 | 6,140, 129 | |
| Time in the frees | 2.437 s | 0.036 s | |
| Sleeps for queue room | 13, 0.239 s | 0 | |
| Notifies, their time | 50,018, 8.79 s | 28,303, 7.77 s | |
| Notifies inside a reap | 4,487 | 0 | |
| GEM_CLOSE time in all | 2.068 s | 5.367 s | |
| RESOURCE_CREATE time in all | 8.797 s | 3.881 s | |

The creates now match Linux's: Mesa's cache reuses 3,732 buffers it made again before, after 3,735 NOWAIT WAITs that each found the buffer idle. The time moved from RESOURCE_CREATE, whose sleeps for room in the queue are gone, to GEM_CLOSE, whose first DETACH after each flush waits 0.90 to 1.04 s for the host to finish the submission (the longest notify, 1.04 s).

The UNREFs after each submission:

| Submission | At s | UNREFs | First, after the submission | Burst's length |
|---|---|---|---|---|
| 1 | 0.000 | 1,252 | 906.7 ms | 292.4 ms |
| 2 | 2.147 | 1,266 | 986.6 ms | 251.1 ms |
| 3 | 4.342 | 1,270 | 1,039.2 ms | 279.3 ms |
| 4 | 6.648 | 1,251 | 980.0 ms | 304.3 ms |
| 6 | 8.279 | 1,230 | 4.5 ms | 303.3 ms |

The task's mark of a submission's UNREFs within 0.1 s is missed: they spread over 0.25 to 0.30 s against Linux's 0.03 to 0.04 s. The worker no longer sets the pace, since it frees an answered batch with one notify; the UNREFs follow the test's own GEM_CLOSEs, each of which sends its DETACH with a notify, about 0.24 ms a close after the first (1,251 closes in 0.295 s after the first close's 0.903 s). Rach2 already leaves out a notify the device suppresses. What Linux's close costs under the same QEMU was not measured here.

1,227 of the after run's GEM_CLOSEs fail with EBADF, against 2 before. All come after the last submission, from Mesa's teardown: `virgl_drm_screen_destroy` in Mesa 26.1.6 closes the winsys's descriptor and then destroys the screen, whose `virgl_drm_winsys_destroy` flushes the buffer cache with a GEM_CLOSE for each buffer on the closed descriptor. The file's release then frees them; `vgpu_m_objects` reads 0 at the end. More buffers sit in the cache at exit in the after run; before, 1,229 of the last 1,231 closes came ahead of the descriptor's close.

## The probe

`vgpuprobe` adds three rows to `machine.vgpu.virgl`: a buffer shared by two files, whose close in one detaches that file's context alone; a close while a mapping and a submission hold the buffer, which is freed after the last of them; and a buffer closed with no GPU ioctl after, which `virtio_gpu_objects` in `/proc/stat` shows freed. The last row prints its time and does not judge it. Under KVM, `tests/run vgpu` passed on i686 (virgl with the rows, the UNREF seen in 12 ms, and exit), and `ARCH=x86_64 tests/run vgpu` passed blob, virgl (2 ms) and exit. `machine.virgl.piglit-sample` waits for the review.

## The rest of the UNREFs' gap

Subtask 5701 measured the 0.25 to 0.30 s against Linux's 0.03 to 0.04 s call by call, on both guests under the same QEMU 10.2.1, and closed it at its cause.

### Setup

The same machine and tools, with the counters of [`vgpu-close/counters-close.patch`](vgpu-close/counters-close.patch) on `task/5697` at 5d957c939, which `/proc/stat` shows as `vgpu_m_*`: the close hooks that sent a DETACH and their time, split into the wait for room and the send; each notify asked for and each the device was given, with their time; and, for a close's notify, the device's write and the retake of the big lock after it. QEMU traced the events of [`vgpu-close/events`](vgpu-close/events), which add to task 5676's the context's attach and detach and the notifies QEMU takes itself (`virtio_pci_notify_write`, since virtio-gpu has no ioeventfd); copied over `dev-log/86-shared-region/measure/events`, it is what `rach2.py` and `debian.sh` pass to QEMU. [`vgpu-close/events-queue`](vgpu-close/events-queue) adds every queue's pops, answers and interrupts and QEMU's fences, and slows QEMU enough to change how many notifies it is given, so its runs are compared only with each other. [`vgpu-close/hostcost.py`](vgpu-close/hostcost.py) reads a trace: QEMU traces each command as it begins to run it and runs a popped batch one command after the other, so the time from one command to the next is the first one's cost; and, for the second to fourth submissions, the burst of closes after each, from its second DETACH to its last UNREF, with its notifies.

    python3 dev-log/86-shared-region/measure/rach2.py build/runs/t5701/rach2-1
    sh dev-log/86-shared-region/measure/debian.sh build/runs/t5701/debian-1
    python3 dev-log/84-locks/vgpu-close/hostcost.py build/runs/t5701/rach2-1/trace

### Before the fix

The runs that ended at 15:34 (Rach2) and 15:35 (Debian 6.12) on 2026-11-22 (UTC+6), both passing:

| | Rach2 | Linux 6.12 |
|---|---|---|
| QEMU's time for a CTX_DETACH_RESOURCE, median | 86 us | 3.1 us |
| QEMU's time for a RESOURCE_UNREF, median | 98 us | 8.1 us |
| A burst of 1,260 closes, second to fourth submission | 279 to 333 ms | 50 to 55 ms |
| Notifies in it, a DETACH | 0.55 to 0.77 | 0.28 to 0.35 |
| GEM_CLOSEs, their time | 6,281, 5.416 s | 6,269, 4.576 s |

The queue run of 15:37 gave 77 and 99 us; Debian's of 15:38, 4.1 and 9.1 us.

Rach2 made about twice Linux's notifies, but its bursts were five to six times as long, so the notifies were not the main cost. QEMU took 20 to 30 times as long over each of Rach2's DETACHes and UNREFs as over Linux's. Rach2 sent every control command with VIRTIO_GPU_FLAG_FENCE but the few `vgpu_cmd_unfence` named. Linux sends a DETACH and an UNREF without one. For a fenced command QEMU makes a virgl fence on its main thread, and it answers the command only at its next poll of virgl's fences: in the queue run QEMU wrote the answers to a popped batch of DETACHes together after the whole batch had run, while Linux's batch ran 4 us a command.

### The fix

`vgpu_obj_close` and `vgpu_obj_free` send the DETACH and the UNREF unfenced (`vgpu_cmd_unfence`), as Linux's `virtio_gpu_gem_object_close` and `virtio_gpu_cmd_unref_resource` queue them. Nothing waits on either command's fence. The first DETACH after a flush still waits for the submission in its notify, which keeps Mesa's cache as it was.

### After the fix

The runs that ended at 15:40 and 15:41, both passing, and the queue run of 15:39:

| | Rach2 before | Rach2 after | Linux 6.12 |
|---|---|---|---|
| QEMU's time for a DETACH, median | 86 us | 9.1 us | 3.1 us |
| QEMU's time for an UNREF, median | 98 us | 11.0 to 11.9 us | 8.1 us |
| The same in the queue runs | 77, 99 us | 4.1, 9.1 us | 4.1, 9.1 us |
| A submission's UNREFs, first to last (`bursts.py`) | 272 to 368 ms | 68 to 90 ms | 39 to 68 ms |
| A burst of closes, second to fourth submission | 279 to 333 ms | 74 to 86 ms | 46 to 69 ms |
| Notifies in it, a DETACH | 0.55 to 0.77 | 0.48 to 0.52 | 0.23 to 0.45 |
| RESOURCE_CREATEs | 6,281 | 6,270, 6,264 | 6,269, 6,274 |
| Peak resources (`trace.py`) | 2,524 | 2,524 | 2,525 |
| RESOURCE_CREATE time a call | 0.618 ms | 0.589, 0.612 ms | 0.612, 0.621 ms |

The Linux figures are from the runs of 15:35 and 15:44. The queue runs, whose tracing slows QEMU, gave a submission's UNREFs over 56 to 78 ms on Rach2 after the fix against 36 to 88 ms on Linux.

A submission's UNREFs now reach the host over 68 to 90 ms. Linux's took 39 to 68 ms in these runs, and 30 to 40 ms in task 5676's. The rest of the gap is the notifies. A close's own time is its notify: of the 4.43 s in the close hooks of the run of 15:41, 4.42 s went in the device's write and 0.058 s in retaking the big lock after it, and the four first closes after a flush waited 0.91 to 1.01 s each for the submission, as Linux's waited 0.89 to 1.08 s. In a burst a notify takes 0.12 to 0.16 ms on either guest, an exit of the closing processor under WSL's nested KVM, and Rach2 made 0.48 to 0.52 a DETACH against Linux's 0.23 to 0.45. Each DETACH's answer wakes Rach2's worker at once, and its pass sends its few UNREFs with a notify of its own. Linux's UNREFs came in runs of up to some 20 within one popped batch, 10 us apart, as its free work sends at once what its dequeue work has gathered. Making Rach2's worker wait to gather more would delay every free, so it is left as it is.

QEMU's fence trace of the after queue run shows fences on RESOURCE_CREATE_3D (0x204), ATTACH_BACKING (0x106), CTX_ATTACH_RESOURCE (0x202) and the submissions (0x207); Debian's run of 15:44 shows fences on RESOURCE_CREATE_3D and the submissions alone. The two further fences cost QEMU its time on each create, outside the bursts of closes, and RESOURCE_CREATE takes as long as Linux's; `VIRTGPU_RESOURCE_CREATE` puts the attach's fence in the buffer's reservation, so taking it off would change what a WAIT waits on. They stay, as a known difference from Linux.

Under KVM with the fix, `tests/run vgpu` passed on i686, virgl with the three rows and exit, the UNREF seen in 2 ms against 12 ms before, and `ARCH=x86_64 tests/run vgpu` passed blob, virgl, 1 ms against 2 ms, and exit.
