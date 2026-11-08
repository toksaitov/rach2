# Task: round 3 argument, Codex

You reviewed the Rach2 graphics plan in round 3 (your report: ../codex.md). The chat and Fable each wrote their own (../position-chat.md, ../fable.md). The chat compared the three in ../comparison.md, settled some facts (F1 to F7) and proposed resolutions (D1 to D10). The task, the owner's order and the machines are in ../brief.md. Read all five files first.

The owner's order: a plan that delivers (1) a solid graphics stack that lets users browse the web and play games, and (2) a test bed for GPU VM proxification research. No dates and no deadlines; quality of the goals first, but pragmatic, not the other extreme. The workers are LLMs and write code fast; plan by scope and proof, not by human effort.

Do this:

1. For each of D1 to D10, write CONCEDE, HOLD or AMEND, with the evidence (a path and line, or a URL you read) and, for HOLD or AMEND, the exact change you want.
2. For each of F1 to F7, say if you dispute it, with evidence.
3. Research on the web and settle what you can of these open questions, citing what you read:
   * Q1. How Mesa 26.1's Crocus native context carries gen7 relocations (execbuffer2 without softpin), and whether virglrenderer's drm-intel server (the `drm-intel-experimental` option) accepts Haswell or only Tiger Lake and newer. Read the code if you can reach it (the GitHub mirrors of Mesa and virglrenderer may work where gitlab.freedesktop.org blocks).
   * Q2. Whether any virtio-gpu device backend that runs as a separate process supports blob resources, context init and Venus today (QEMU vhost-user-gpu, rust-vmm vhost-device-gpu, crosvm's gpu device), and the state of vhost-user shared-memory mapping (SHMEM_MAP) in QEMU 10 and 11.
   * Q3. The best existing basis for the reverse transport (the host uses a GPU its guest owns) with QEMU 10.0 unchanged: ivshmem-plain or ivshmem-doorbell with a Rach2 peer, virtio-msg, virtio-vhost-user, or something else; and how a Rach2 kernel driver gets its doorbells and interrupts from QEMU's ivshmem-server protocol.
   * Q4. Anything in the three reports that is wrong and that nobody caught.
4. Write your answer to rebuttal.md in this directory as you go: section 1 the D items, section 2 the F items, section 3 the questions, section 4 anything else. Plain prose, ASCII only, no em-dashes, `*` bullets, no wrapped lines.

Keep notes in notes.md in this directory as you work. If your context is compacted, read notes.md and this file again and go on; never stop to ask what the task is. Change no file outside this directory. Run no build and touch no machine; reading files and the web is enough.
