# Task 5697: virtio-gpu's DETACH at GEM close and prompt frees

Task 5676 measured piglit's streaming-texture-leak through Linux 6.12's virtio-gpu and through Rach2's under the same QEMU and virgl. Rach2's RESOURCE_UNREFs after each submission reached the host over 0.74 to 0.79 s, against Linux's 0.03 to 0.04 s, and Rach2 made 10,001 RESOURCE_CREATEs against Linux's 6,266. This record measures where Rach2's time goes before any change, and what the change does to it.

## Setup

The testing PC under WSL, one guest, KVM, 8 CPUs and 8 GiB, the `virgl` display profile, with task 5676's tools from `dev-log/86-shared-region/measure/` on `task/5676`: `rach2.py` boots `gnu-test.disk` of `milestone/76-display` with a kernel of the same head, runs `stl.sh` as the test account and traces QEMU's resource and submission events. The kernel carries task 5676's counters and the ones in [`vgpu-close/counters.patch`](vgpu-close/counters.patch), which `/proc/stat` shows as `vgpu_m_*`:

* `vgpu_m_sub_worker` and `vgpu_m_sub_other`: submissions whose answer the worker reaped, and those another thread reaped; `vgpu_m_sub_lag_ns` and `_max_ns`, the time from the answer's interrupt to the reap of the submission; `vgpu_m_sub_done_ns`, the time in its completion, which drops the submission's references to its buffers.
* `vgpu_m_free_worker` and `vgpu_m_free_other`: objects freed (`vgpu_obj_free`) in the worker and elsewhere; `vgpu_m_free_ns`, the time in those frees; `vgpu_m_detach_ns`, the part of it in `vgpu_resource_detach`; `vgpu_m_release_ns`, the time in `obj_release` after the UNREF's answer.
* `vgpu_m_reap_notifies` and `_ns`: notifies made inside a reap, and their time.

[`vgpu-close/bursts.py`](vgpu-close/bursts.py) reads QEMU's trace and gives, for each submission, the UNREFs up to the next one, grouped into bursts whose UNREFs come less than 50 ms apart.

    sh: make ARCH=x86_64 rach2 build/x86_64/images/gnu-test.disk
    sh: python3 dev-log/86-shared-region/measure/rach2.py build/runs/task-5697/before-1
    sh: python3 dev-log/84-locks/vgpu-close/bursts.py build/runs/task-5697/before-1/trace

## Before the change

The run at 15:02 on 2026-11-22 (UTC+6): pass, in 17.1 s, with 10,001 creates, 10,001 UNREFs, a peak of 2,525 resources and 1,291,760 pages, as task 5676 found.

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
