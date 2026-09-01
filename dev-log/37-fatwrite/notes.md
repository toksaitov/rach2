# Milestone 37 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 36.

```text
207fb2a Add the full set's time to the milestone 36 record
64829b4 Record milestone 36
7262391 Correct the mount documents and the VFS header against the code
b1bd997 Remove the KRFFS walk predicate and resolve readlink through the mount table
43fbb0e Record the review round and the contracts it changed
1f5c1d9 Accept a debug-exit status under --ok and expect EISDIR for creat of the root
8054353 Make the probe rows prove what their names claim
70c9380 Validate long names, times and options in vfat and prove them on the host
813b0aa Retry failed writeback and check buffer ownership in the cache
889a956 Fix the review findings in the VFS and the syscall layer
cacbdd7 Own a request by generation, claim ranges on the physical disk, and stage lagged reads
ba29673 Describe the mount table, the vfat type and the block request in their Readmes
```

## Standing rule changed by the owner

The plan's rule of five concurrent delegated units is superseded: at most
two run at once (CLAUDE.md, Agents), set during milestone 36's review round.

## Open items

(none)
