# Apply the test kernel's one-thread profile to the tree; `git checkout` the two files undoes it.
p='rach2/sys/arch/x86/isr.c'
s=open(p).read()
old="""    } else if (params->vector == IRQ_VECTOR(0) && !FRAME_USER(params)) {
        bkl_profile_sample((uintptr_t)FRAME_PC(params), fp);
    }"""
new=old[:-6]+"""    } else if (params->vector == IRQ_VECTOR(0)) {
        bkl_profile_sample(1, 0);
    }"""
assert old in s; s=s.replace(old,new); open(p,'w').write(s)
p='rach2/sys/proc/smp.c'
s=open(p).read()
old="""    if (!bkl_profile_whole && cpu->bkl_depth < 1) {
        return;
    }
    sample_chain(chain, pc, fp, cpu->current ? cpu->current->kernel_stack : 0);"""
new="""    if (pc == 1 && !bkl_profile_tid) {
        return;
    }
    if (bkl_profile_tid && (!cpu->current || cpu->current->pid != (uint32_t)bkl_profile_tid)) {
        return;
    }
    if (!bkl_profile_whole && cpu->bkl_depth < 1) {
        return;
    }
    if (pc == 1) {
        chain[0] = 1;
        chain[1] = chain[2] = chain[3] = 0;
    } else
    sample_chain(chain, pc, fp, cpu->current ? cpu->current->kernel_stack : 0);"""
assert old in s; s=s.replace(old,new)
old="""static volatile int bkl_profile_whole;"""
assert old in s; s=s.replace(old,old+"\nstatic volatile int bkl_profile_tid;",1)
old="""    bkl_profile_whole = whole;
    profile_hold(0);"""
new="""    bkl_profile_whole = whole;
    bkl_profile_tid = 0;
    if (whole && len > 7) {
        char digits[12];
        uint32_t n = len - 7 < sizeof(digits) ? len - 7 : sizeof(digits);
        int tid = 0;

        if (copy_from_user(digits, buf + 7, n) == 0) {
            for (i = 0; i < n && digits[i] >= '0' && digits[i] <= '9'; i++) {
                tid = tid * 10 + (digits[i] - '0');
            }
        }
        bkl_profile_tid = tid;
    }
    profile_hold(0);"""
assert old in s; s=s.replace(old,new); open(p,'w').write(s)
