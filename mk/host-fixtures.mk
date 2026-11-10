# Extracted host fixtures use the ordinary compiler and the extractor of
# tests/host/fixtures/extract.c.
HOST_FIXTURE_PREFIX ?= build/tests/host-suite/scripts
HOST_VFS_PREFIX ?= build/tests/host-suite
HOST_KERNEL_COMMON = -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0
ifeq ($(OS),Windows_NT)
HOST_KERNEL_COMMON += -g -O1 -D_WIN32_WINNT=0x0A00 -static
HOST_KERNEL_THREADS =
else
HOST_KERNEL_COMMON += -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1
HOST_KERNEL_THREADS = -pthread
endif
HOST_KERNEL_REPRODUCIBLE = $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-reproducible)
comma := ,

# The extractor that copies kernel definitions into the fixtures' .inc files.
HOST_EXTRACT ?= $(HOST_FIXTURE_PREFIX)/extract
$(HOST_EXTRACT): tests/host/fixtures/extract.c mk/host-fixtures.mk
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O1 -Wall -Wextra -Werror -o $@ tests/host/fixtures/extract.c
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif

$(HOST_FIXTURE_PREFIX)/chown-sources/chown_plan.inc: rach2/sys/fs/vfs.h rach2/sys/lib/kerrno.h rach2/sys/proc/process.h rach2/sys/syscall_fs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/fs/vfs.h '#define VFS_SET_' 'struct vfs_stat' -f rach2/sys/lib/kerrno.h '#define EPERM' -f rach2/sys/proc/process.h '#define proc_privileged' -f rach2/sys/syscall_fs.c cred_privileged cred_in_suppl cred_in_group vfs_chown_plan

$(HOST_FIXTURE_PREFIX)/chown: tests/host/fixtures/chown_plan.c $(HOST_FIXTURE_PREFIX)/chown-sources/chown_plan.inc $(HOST_EXTRACT) rach2/sys/fs/vfs.h rach2/sys/lib/kerrno.h rach2/sys/proc/process.h rach2/sys/syscall_fs.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/chown-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/chown-sources -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/chown-sources/00-chown_plan.o.d -MT $@ -c tests/host/fixtures/chown_plan.c -o $(HOST_FIXTURE_PREFIX)/chown-sources/00-chown_plan.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/chown-sources/00-chown_plan.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/chown-sources/00-chown_plan.o.d

$(HOST_FIXTURE_PREFIX)/krffs_open-sources/vfs_open.inc: rach2/sys/fs/vfs.c rach2/sys/fs/sleeplock.c rach2/sys/fs/nodelock.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/fs/sleeplock.c 'struct sleep_turn' sleep_caller sleep_my_turn sleep_lock_take sleep_record_hold sleep_lock_give sleep_lock_held -f rach2/sys/fs/vfs.c vfs_mount_gone mount_base mount_rdonly vfs_ref_hold vfs_ref_put vfs_enter vfs_leave vfs_ns_lock vfs_ns_unlock in_groups may_search toward_cwd mount_covering path_last mount_named_at mount_named named_in_rest climbs node_put walk_lock walk_unlock vfs_walk_unlock ref_move_node walk_move walk_cross walk_root root_current dotdot_stays walk_leaf walk_pages walk_splice walk_link walk_last_comp walk_is_link walk_rest_len walk_name_root walk_name_add walk_name_text walk_name_end 'struct walk_record' walk_record_entry walk_record_clear walk_record_move walk_release walk_finish resolve_path vfs_resolve_entry vfs_resolve vfs_ref_release vfs_desc_open vfs_desc_close vfs_getattr vfs_open_node vfs_release vfs_truncate vfs_create -f rach2/sys/fs/nodelock.c base_of vfs_file_locked ref_node vfs_node_lock vfs_node_unlock

$(HOST_FIXTURE_PREFIX)/krffs_open-sources/syscall_open.inc: rach2/sys/syscall_fs.c rach2/sys/syscall_internal.h rach2/sys/syscall_fs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/syscall_fs.c '#define O_|CRED_|S_IF|RECHECK_' -f rach2/sys/syscall_internal.h '#define CRED_' -f rach2/sys/syscall_fs.c name_is device_name open_device_name open_link_target node_make file_lock file_unlock open_truncate dir_recheck dir_lock open_create open_mounted_node path_starts ofd_record_path open_install open_synth_node do_open

$(HOST_FIXTURE_PREFIX)/krffs_open: tests/host/fixtures/krffs_open.c rach2/sys/fs/fd.c tests/host/compat/vfs_name_none.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c tests/host/fixtures/krffs_open_threads.c $(HOST_FIXTURE_PREFIX)/krffs_open-sources/vfs_open.inc $(HOST_FIXTURE_PREFIX)/krffs_open-sources/syscall_open.inc $(HOST_EXTRACT) rach2/sys/fs/vfs.c rach2/sys/fs/nodelock.c rach2/sys/syscall_fs.c rach2/sys/syscall_internal.h $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/krffs_open-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/00-krffs_open.o.d -MT $@ -c tests/host/fixtures/krffs_open.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/00-krffs_open.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/01-fd.o.d -MT $@ -c rach2/sys/fs/fd.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/01-fd.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/02-vfs_name_none.o.d -MT $@ -c tests/host/compat/vfs_name_none.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/02-vfs_name_none.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/03-kstring.o.d -MT $@ -c rach2/sys/lib/kstring.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/03-kstring.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/04-kctypes.o.d -MT $@ -c rach2/sys/lib/kctypes.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/04-kctypes.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_open-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_open-sources/05-krffs_open_threads.o.d -MT $@ -c tests/host/fixtures/krffs_open_threads.c -o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/05-krffs_open_threads.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/krffs_open-sources/00-krffs_open.o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/01-fd.o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/02-vfs_name_none.o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/03-kstring.o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/04-kctypes.o $(HOST_FIXTURE_PREFIX)/krffs_open-sources/05-krffs_open_threads.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/krffs_open-sources/00-krffs_open.o.d $(HOST_FIXTURE_PREFIX)/krffs_open-sources/01-fd.o.d $(HOST_FIXTURE_PREFIX)/krffs_open-sources/02-vfs_name_none.o.d $(HOST_FIXTURE_PREFIX)/krffs_open-sources/03-kstring.o.d $(HOST_FIXTURE_PREFIX)/krffs_open-sources/04-kctypes.o.d $(HOST_FIXTURE_PREFIX)/krffs_open-sources/05-krffs_open_threads.o.d

$(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/kfs_data.inc: rach2/sys/fs/vfs.c rach2/sys/fs/sleeplock.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/fs/sleeplock.c 'struct sleep_turn' sleep_caller sleep_my_turn sleep_lock_take sleep_record_hold sleep_lock_give sleep_lock_held -f rach2/sys/fs/vfs.c mount_base vfs_ns_lock vfs_ns_unlock kfs_read_locked kfs_write_locked kfs_truncate_locked kfs_fsync_locked kfs_readdir_locked kfs_read kfs_write kfs_truncate kfs_fsync kfs_readdir

$(HOST_FIXTURE_PREFIX)/krffs_root_lock: tests/host/fixtures/krffs_root_lock.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c tests/host/fixtures/krffs_open_threads.c $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/kfs_data.inc $(HOST_EXTRACT) rach2/sys/fs/vfs.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/00-krffs_root_lock.o.d -MT $@ -c tests/host/fixtures/krffs_root_lock.c -o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/00-krffs_root_lock.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/01-kstring.o.d -MT $@ -c rach2/sys/lib/kstring.c -o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/01-kstring.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/02-kctypes.o.d -MT $@ -c rach2/sys/lib/kctypes.c -o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/02-kctypes.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc $(HOST_KERNEL_THREADS) -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/03-krffs_open_threads.o.d -MT $@ -c tests/host/fixtures/krffs_open_threads.c -o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/03-krffs_open_threads.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/00-krffs_root_lock.o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/01-kstring.o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/02-kctypes.o $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/03-krffs_open_threads.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/00-krffs_root_lock.o.d $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/01-kstring.o.d $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/02-kctypes.o.d $(HOST_FIXTURE_PREFIX)/krffs_root_lock-sources/03-krffs_open_threads.o.d

$(HOST_FIXTURE_PREFIX)/krffs_stat-sources/krffs_stat_types.inc: rach2/sys/arch/i686/uabi.h rach2/sys/fs/vfs.h rach2/sys/arch/i686/uabi.h rach2/sys/syscall_fs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/arch/i686/uabi.h 'struct linux_stat' 'struct user_stat' -f rach2/sys/fs/vfs.h 'struct vfs_stat' -f rach2/sys/arch/i686/uabi.h '#define STAT_SET_INO' -f rach2/sys/syscall_fs.c 'struct kstat'

$(HOST_FIXTURE_PREFIX)/krffs_stat-sources/krffs_stat_funcs.inc: rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/syscall_fs.c path_names_dir stat64_put stat64_node_times stat64_root_times krffs_fill_stat64 stat_times_from_stat64 stat_node_times stat_root_times do_stat64 old_stat_put do_old_stat -f rach2/sys/fs/vfs.c kfs_getattr

$(HOST_FIXTURE_PREFIX)/krffs_stat: tests/host/fixtures/krffs_stat.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/krffs_stat_types.inc $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/krffs_stat_funcs.inc $(HOST_EXTRACT) rach2/sys/arch/i686/uabi.h rach2/sys/fs/vfs.h rach2/sys/syscall_fs.c rach2/sys/fs/vfs.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/krffs_stat-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -I $(HOST_FIXTURE_PREFIX)/krffs_stat-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/00-krffs_stat.o.d -MT $@ -c tests/host/fixtures/krffs_stat.c -o $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/00-krffs_stat.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -I $(HOST_FIXTURE_PREFIX)/krffs_stat-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/01-kstring.o.d -MT $@ -c rach2/sys/lib/kstring.c -o $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/01-kstring.o
	$(HOSTCC) $(HOST_KERNEL_COMMON) -I $(HOST_FIXTURE_PREFIX)/krffs_stat-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/02-kctypes.o.d -MT $@ -c rach2/sys/lib/kctypes.c -o $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/02-kctypes.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/00-krffs_stat.o $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/01-kstring.o $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/02-kctypes.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/00-krffs_stat.o.d $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/01-kstring.o.d $(HOST_FIXTURE_PREFIX)/krffs_stat-sources/02-kctypes.o.d

$(HOST_FIXTURE_PREFIX)/pagecache-sources/syscall_sync.inc: rach2/sys/syscall_fs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/syscall_fs.c sys_sync

$(HOST_FIXTURE_PREFIX)/pagecache: tests/host/pagecache_hosttest.c $(HOST_FIXTURE_PREFIX)/pagecache-sources/syscall_sync.inc $(HOST_EXTRACT) rach2/sys/syscall_fs.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/pagecache-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/pagecache-sources -DRACH2_HOSTTEST -ffreestanding -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -iquote rach2/sys -iquote rach2/sys/lib -iquote rach2/sys/proc -DPAGECACHE_SYNC_HOSTTEST -Wno-empty-body -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 -I utils -I krffs -I tests/host/compat -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/pagecache-sources/00-pagecache_hosttest.o.d -MT $@ -c tests/host/pagecache_hosttest.c -o $(HOST_FIXTURE_PREFIX)/pagecache-sources/00-pagecache_hosttest.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/pagecache-sources/00-pagecache_hosttest.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/pagecache-sources/00-pagecache_hosttest.o.d

$(HOST_FIXTURE_PREFIX)/kstack-sources/kstack.inc: rach2/sys/proc/process.h rach2/sys/arch/i686/cpu.h rach2/sys/syscall_proc.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/proc/process.h '#define KERNEL_STACK_SIZE' -f rach2/sys/arch/i686/cpu.h '#define PAGE_SIZE' -f rach2/sys/syscall_proc.c kstack_alloc kstack_free kstack_overrun_pid

$(HOST_FIXTURE_PREFIX)/kstack: tests/host/fixtures/kstack.c $(HOST_FIXTURE_PREFIX)/kstack-sources/kstack.inc $(HOST_EXTRACT) rach2/sys/proc/process.h rach2/sys/arch/i686/cpu.h rach2/sys/syscall_proc.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/kstack-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/kstack-sources -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/kstack-sources/00-kstack.o.d -MT $@ -c tests/host/fixtures/kstack.c -o $(HOST_FIXTURE_PREFIX)/kstack-sources/00-kstack.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/kstack-sources/00-kstack.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/kstack-sources/00-kstack.o.d

$(HOST_FIXTURE_PREFIX)/endothers-sources/endothers.inc: rach2/sys/syscall_proc.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/syscall_proc.c proc_end_others

$(HOST_FIXTURE_PREFIX)/endothers: tests/host/fixtures/endothers.c $(HOST_FIXTURE_PREFIX)/endothers-sources/endothers.inc $(HOST_EXTRACT) rach2/sys/syscall_proc.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_FIXTURE_PREFIX)/endothers-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_FIXTURE_PREFIX)/endothers-sources -MMD -MP -MF $(HOST_FIXTURE_PREFIX)/endothers-sources/00-endothers.o.d -MT $@ -c tests/host/fixtures/endothers.c -o $(HOST_FIXTURE_PREFIX)/endothers-sources/00-endothers.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_FIXTURE_PREFIX)/endothers-sources/00-endothers.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_FIXTURE_PREFIX)/endothers-sources/00-endothers.o.d

$(HOST_VFS_PREFIX)/vfs_unmount-sources/unmount.inc: rach2/sys/fs/vfs.c $(HOST_EXTRACT)
	@mkdir -p $(@D)
	$(HOST_EXTRACT) $@ -f rach2/sys/fs/vfs.c streq vfs_mount_gone mount_base mount_permanent unmount_abandoned unmount_target unmount_below_gone unmount_found vfs_do_umount unmount_bind unmount_mount

$(HOST_VFS_PREFIX)/vfs_unmount: tests/system/fixtures/vfs_unmount.c $(HOST_VFS_PREFIX)/vfs_unmount-sources/unmount.inc $(HOST_EXTRACT) rach2/sys/fs/vfs.c $(HOST_KERNEL_FIXTURE_INPUTS)
	@mkdir -p $(HOST_VFS_PREFIX)/vfs_unmount-sources
	$(HOSTCC) $(HOST_KERNEL_COMMON) -Wall -Wextra -Werror -I $(HOST_VFS_PREFIX)/vfs_unmount-sources -MMD -MP -MF $(HOST_VFS_PREFIX)/vfs_unmount-sources/00-vfs_unmount.o.d -MT $@ -c tests/system/fixtures/vfs_unmount.c -o $(HOST_VFS_PREFIX)/vfs_unmount-sources/00-vfs_unmount.o
	$(HOSTCC) $(filter -fsanitize=% -g -pthread -static,$(HOST_KERNEL_COMMON) $(HOST_KERNEL_THREADS)) $(HOST_KERNEL_REPRODUCIBLE) -o $@ $(HOST_VFS_PREFIX)/vfs_unmount-sources/00-vfs_unmount.o
ifeq ($(OS),Windows_NT)
	@if test -f $@.exe; then mv $@.exe $@; fi
endif
-include $(HOST_VFS_PREFIX)/vfs_unmount-sources/00-vfs_unmount.o.d
