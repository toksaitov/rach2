/*
 * devtools_scenario.c: the scenario of machine.tcc and machine.make
 * (tests/cases/machine.tcc.json and tests/cases/machine.make.json), the
 * installed system compiling programs on itself, as the unprivileged test
 * account at the serial console. Case `compile` compiles and runs a program
 * with tcc, requires each output kind the system cannot run to be refused
 * with its reason, builds two of this project's programs, runs one whose
 * x86-64 code must clear or extend the high half of a 32-bit value, and
 * reads the kernel's trace of missing system calls once the machine has
 * ended. Case `rebuild` builds a program with make, rebuilds what a change
 * needs and nothing else, and does it again after a reboot of the same disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The test account's shell prompt. */
#define PROMPT "$ "

/* What each typed line prints last, whatever it found, so that the next line
 * is typed only once the one before it has ended. */
#define LINE_END "LINE_END"

/* The longest line typed, well below the 4095 bytes of a line the console's
 * line discipline keeps (N_TTY_LINE_MAX in rach2/sys/tty/n_tty.h). */
#define TYPED_MAX 240

/* One attempt, its boot, and what the last line typed printed. */
struct shell {
    struct net_case u;
    struct net_boot b;
    char password[128];
    char *out; /* the console from the typed line to its LINE_END, or NULL */
};

/**
 * Boot the machine as its boot `n` and log in at the console as the test
 * account. Assertion `id` holds when the shell's prompt comes.
 */
static void login(struct shell *s, unsigned n, const char *id)
{
    net_boot(&s->b, &s->u, "fixture", n, NULL);
    net_login(&s->b, "test", s->password, PROMPT);
    net_assert(&s->u, id, 1, "the test account did not log in at the console");
}

/**
 * Type `command` and wait for LINE_END, which the scenario adds after it,
 * and then for the prompt, keeping in `s->out` what the line printed. Each
 * token the command prints is written split in it, `BUILT""IT`, so that the
 * echo of the line is not taken for it. A line longer than TYPED_MAX is a
 * fault of the scenario.
 */
static void type(struct shell *s, const char *command)
{
    char line[TYPED_MAX + 1];

    free(s->out);
    s->out = NULL;
    if (!net_ok(&s->u))
        return;
    if (snprintf(line, sizeof(line), "%s; echo LINE\"\"_END", command) >= (int)sizeof(line)) {
        net_fail(&s->u, "the line is too long for the console: %.60s", command);
        return;
    }
    if (net_command(&s->b, line, LINE_END, &s->out) == ETUDE_OK)
        net_wait(&s->b, PROMPT);
}

/** Report assertion `id`: it holds when the last line printed `token`. */
static void has(struct shell *s, const char *id, const char *token, const char *why)
{
    net_assert(&s->u, id, s->out != NULL && strstr(s->out, token) != NULL, "%s", why);
}

/** Fail the attempt with `why` unless the last line printed `token`, which
 * the lines after it need. */
static void need(struct shell *s, const char *token, const char *why)
{
    if (net_ok(&s->u) && (s->out == NULL || strstr(s->out, token) == NULL))
        net_fail(&s->u, "%s", why);
}

/** Give the number the last line printed after `prefix`, or -1. */
static long number(const struct shell *s, const char *prefix)
{
    return net_number_after(s->out, prefix);
}

/** Type `sync` and require the shell to come back; then end the boot with
 * QMP's quit. Assertion `id` holds when the sync returned. */
static void finish(struct shell *s, const char *id)
{
    struct etude_termination end;
    char *reply = NULL;

    type(s, "sync; echo SYNC\"\"ED");
    has(s, id, "SYNCED", "the boot never finished its lines");
    if (net_ok(&s->u)) {
        if (etude_qmp(s->b.m, "quit", NULL, &reply) != ETUDE_OK ||
            etude_wait_end(s->b.m, &end) != ETUDE_OK)
            net_fail(&s->u, "the boot did not end");
        free(reply);
    }
}

/*
 * The compile case's lines. The program is written with printf, a line at a
 * time, and the refusals each print the status and then what the compiler
 * said, so that both are judged.
 */
static const char hello_line[] =
    "printf '#include <stdio.h>\\nint main(void){printf(\"COMPILED\"\"HERE\\\\n\");"
    "return 0;}\\n' > /tmp/hello.c";
static const char built_line[] = "tcc -static -o /tmp/hello /tmp/hello.c && echo BUILT\"\"IT";
static const char run_line[] = "tcc -run /tmp/hello.c > /tmp/run.out 2>&1; echo RUN\"\"RC=$?; "
                               "cat /tmp/run.out";
static const char shared_line[] = "tcc -shared -o /tmp/lib.so /tmp/hello.c > /tmp/sh.out 2>&1; "
                                  "echo SH\"\"RC=$?; cat /tmp/sh.out";
static const char dynamic_line[] = "tcc -o /tmp/dyn /tmp/hello.c > /tmp/dy.out 2>&1; "
                                   "echo DY\"\"RC=$?; cat /tmp/dy.out";
static const char raw_line[] = "tcc -static -Wl,-oformat=binary -o /tmp/raw /tmp/hello.c > "
                               "/tmp/rw.out 2>&1; echo RW\"\"RC=$?; cat /tmp/rw.out";
static const char leftover_line[] =
    "[ ! -e /tmp/lib.so ] && [ ! -e /tmp/dyn ] && [ ! -e /tmp/raw ] "
    "&& echo NO\"\"LEFTOVERS";
static const char object_line[] = "tcc -c -o /tmp/hello.o /tmp/hello.c && echo OBJECT\"\"OK";
static const char tool_line[] = "tcc -static -o /tmp/cat /src/utils/cat.c && echo TOOL\"\"BUILT";
static const char input_line[] =
    "busybox dd if=/bin/tcc of=/tmp/in.bin bs=1024 count=21 2>/dev/null "
    "&& echo INPUT\"\"MADE";
static const char same_line[] = "/tmp/cat /tmp/in.bin > /tmp/mine.out && busybox cat /tmp/in.bin > "
                                "/tmp/theirs.out && cmp /tmp/mine.out /tmp/theirs.out && "
                                "echo SAME\"\"BYTES";
static const char both_line[] = "/tmp/cat /tmp/in.bin /tmp/hello.c > /tmp/mine2.out && busybox cat "
                                "/tmp/in.bin /tmp/hello.c > /tmp/theirs2.out && cmp /tmp/mine2.out "
                                "/tmp/theirs2.out && echo BOTH\"\"FILES";
static const char units_line[] =
    "tcc -static -o /tmp/ping /src/utils/ping.c /src/utils/resolv.c && "
    "echo TWO\"\"UNITS";

/*
 * A program that reads an unsigned int out of a 64-bit register on x86-64.
 * The conditional expression leaves 0x100000001 in a register, and the cast
 * to unsigned int emits no code there, so a conversion to floating point
 * that read the whole register gave 4294967297 in place of 1. The size of a
 * row of a variable-length array is a size_t, and a pointer to a row steps
 * by it: v steps from the last row of three back one, which an int size
 * scaled in 32 bits moved forward almost 4 GiB, and w steps one row of 2 GiB
 * forward in a mapping that reserves no memory, which an int size read back
 * negative moved backward; the x86-64 step by the size of a pointer gave 16
 * and 8. The program the i686 compiler makes prints the same line.
 */
static const char wide1_line[] =
    "printf '#include <stdio.h>\\n#include <sys/mman.h>\\ntypedef unsigned long long u64;\\n"
    "double d(u64 a,int c){return (unsigned)(c?a:0);}\\n' > /tmp/wide.c";
static const char wide2_line[] = "printf 'float f(u64 a,int c){return (unsigned)(c?a:0);}\\n"
                                 "long double l(u64 a,int c){return (unsigned)(c?a:0);}\\n' >> "
                                 "/tmp/wide.c";
static const char wide3_line[] = "printf 'long v(int n,int i){int a[3][n];int(*p)[n]=a+2;"
                                 "return (char*)(p+i)-(char*)a;}\\n' >> /tmp/wide.c";
static const char wide4_line[] = "printf 'unsigned long w(unsigned n){char*m=mmap(0,n,PROT_NONE,"
                                 "MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);' >> /tmp/wide.c";
static const char wide5_line[] =
    "printf 'char(*p)[n]=(void*)m;if(m==MAP_FAILED)return 0;"
    "return (unsigned long)(p+1)-(unsigned long)m;}\\n' >> /tmp/wide.c";
static const char wide6_line[] =
    "printf 'int main(void){u64 a=0x100000001ULL;printf(\"WIDE\"\"NED %%lld %%lld %%lld "
    "%%ld %%lu\\\\n\",(long long)d(a,1),(long long)f(a,1),(long long)l(a,1),v(3,-1),"
    "w(0x80000000u));"
    "return 0;}\\n' >> /tmp/wide.c";
static const char wide_built_line[] = "tcc -static -o /tmp/wide /tmp/wide.c && echo WIDE\"\"BUILT";

/*
 * A program whose array parameters are sized by earlier parameters, as C99
 * allows: f sums int a[n]; g weights the rows of int a[n][m] and adds
 * sizeof a[0], the row's 12 bytes, which a[i][j] steps by; h is declared
 * with int a[*] before main calls it and defined after with int a[n].
 * GCC's program prints VLAPARAM 46 3612 40 on both targets. TinyCC 0.9.27
 * stopped at f with 'n' undeclared.
 */
static const char vla1_line[] =
    "printf '#include <stdio.h>\\nlong f(int n,int a[n]){long s=0;int i;"
    "for(i=0;i<n;i++)s+=a[i];return s;}\\n' > /tmp/vparm.c";
static const char vla2_line[] = "printf 'long g(int n,int m,int a[n][m]){long s=0;int i,j;"
                                "for(i=0;i<n;i++)for(j=0;j<m;j++)' >> /tmp/vparm.c";
static const char vla3_line[] = "printf 's+=a[i][j]*(i+1);return s*100+sizeof a[0];}\\n"
                                "void h(int n,int a[*]);\\n' >> /tmp/vparm.c";
static const char vla4_line[] = "printf 'int main(void){int x[4]={1,2,3,4};"
                                "int y[2][3]={{1,2,3},{4,5,6}};h(4,x);' >> /tmp/vparm.c";
static const char vla5_line[] = "printf 'printf(\"VLA\"\"PARAM %%ld %%ld %%d\\\\n\",f(4,x),"
                                "g(2,3,y),x[3]);return 0;}\\n' >> /tmp/vparm.c";
static const char vla6_line[] = "printf 'void h(int n,int a[n]){a[n-1]=n*10;}\\n' >> /tmp/vparm.c";
static const char vla_built_line[] = "tcc -static -o /tmp/vparm /tmp/vparm.c && echo VLA\"\"BUILT";

/**
 * Report a refusal of the compiler, which the last line asked for: assertion
 * `id` that the status `status` was reported, and `reason_id` that the
 * compiler said `reason`, since a refusal for the wrong reason tells the
 * student something false about the machine.
 */
static void refused(struct shell *s, const char *id, const char *status, const char *reason_id,
                    const char *reason, const char *what)
{
    char why[160];

    snprintf(why, sizeof(why), "%s was not refused", what);
    has(s, id, status, why);
    snprintf(why, sizeof(why), "%s was refused without saying why", what);
    has(s, reason_id, reason, why);
}

/**
 * Run case `compile`, as the test account and not as root, since a compiler
 * on the machine must work for an ordinary user: the sysroot must be
 * readable by one, and the program written where one may write.
 *
 * A refused compile must leave no output, which the guest checks. Compiling
 * to an object is not refused: it makes no program to run, and it is how a
 * multi-step build works.
 *
 * The cat the machine builds from /src/utils is judged by what it does, not
 * by its bytes: the shipped one was linked against libgcc on the host and
 * this one against libtcc1.a on the machine. The reference is BusyBox's cat,
 * since this image has no /bin/cat. The input is 21 KB of a binary file, and
 * the second comparison passes two files: a cat that returned after its
 * first 4096-byte read, or stopped after its first argument, would differ.
 * The commands are chained with `&&`, so a cat that failed cannot pass by
 * leaving an old output in place. ping is the one in-house program with a
 * second source, and is linked across both and not run, since the boot has
 * no network.
 *
 * A missing system call says nothing on its own: the kernel answers -ENOSYS
 * in a register. The boot's `rach2.trace=enosys` makes the kernel print a
 * line for each missing number, once for each process, naming the process,
 * so the question is asked of the lines that name tcc, since another
 * program may ask for a call this kernel lacks and does not need. It is
 * asked last, of the whole console of the boot that has ended, so that it
 * sees what every compile asked for.
 */
static void compile(struct shell *s)
{
    char line[200];
    const char *missing;
    char *text;

    net_step(&s->u, "boot");
    login(s, 1, "login");

    net_step(&s->u, "compile");
    type(s, hello_line);
    type(s, "cat /tmp/hello.c");
    type(s, built_line);
    has(s, "built", "BUILTIT", "the compiler did not build a program on the machine");
    type(s, "/tmp/hello");
    has(s, "ran", "COMPILEDHERE",
        "the program the machine compiled did not run, or printed the wrong thing");

    net_step(&s->u, "refuse");
    type(s, run_line);
    refused(s, "run-refused", "RUNRC=1", "run-reason", "cannot reach the C library", "-run");
    type(s, shared_line);
    refused(s, "shared-refused", "SHRC=1", "shared-reason", "shared is not available", "-shared");
    type(s, dynamic_line);
    refused(s, "dynamic-refused", "DYRC=1", "dynamic-reason", "must be linked with -static",
            "a link without -static");
    type(s, raw_line);
    refused(s, "raw-refused", "RWRC=1", "raw-reason", "must be an ELF", "a raw-image link");
    type(s, leftover_line);
    has(s, "no-leftovers", "NOLEFTOVERS", "a refused compile left its output behind");

    net_step(&s->u, "object");
    type(s, object_line);
    has(s, "object", "OBJECTOK", "compiling to an object was refused, and it must not be");

    net_step(&s->u, "tool");
    type(s, tool_line);
    has(s, "tool-built", "TOOLBUILT", "the machine could not compile this project's own cat");
    type(s, input_line);
    has(s, "input-made", "INPUTMADE", "the multi-block input for the comparison was not made");
    type(s, same_line);
    has(s, "same-bytes", "SAMEBYTES",
        "the cat the machine compiled does not agree with the shipped one");
    type(s, both_line);
    has(s, "both-files", "BOTHFILES", "the cat the machine compiled does not agree over two files");

    net_step(&s->u, "units");
    type(s, units_line);
    has(s, "two-units", "TWOUNITS",
        "the machine could not compile a program from two sources at once");

    net_step(&s->u, "widen");
    type(s, wide1_line);
    type(s, wide2_line);
    type(s, wide3_line);
    type(s, wide4_line);
    type(s, wide5_line);
    type(s, wide6_line);
    type(s, "cat /tmp/wide.c");
    type(s, wide_built_line);
    has(s, "widen-built", "WIDEBUILT", "the compiler did not build the program of the widening");
    type(s, "/tmp/wide");
    has(s, "widened", "WIDENED 1 1 1 12 2147483648",
        "the compiler read the high half of a 64-bit register for an unsigned int it converted "
        "to floating point, or scaled an index into a variable-length array by a size held in an "
        "int");

    net_step(&s->u, "vla-param");
    type(s, vla1_line);
    type(s, vla2_line);
    type(s, vla3_line);
    type(s, vla4_line);
    type(s, vla5_line);
    type(s, vla6_line);
    type(s, "cat /tmp/vparm.c");
    type(s, vla_built_line);
    has(s, "vla-built", "VLABUILT",
        "the compiler did not build a program whose array parameters are sized by earlier "
        "parameters");
    type(s, "/tmp/vparm");
    has(s, "vla-param", "VLAPARAM 46 3612 40",
        "an array parameter sized by an earlier parameter did not give GCC's result");

    net_step(&s->u, "end");
    finish(s, "finished");
    text = net_file(s->b.console);
    missing = net_find(text, "not implemented*(asked for by tcc)");
    net_assert(&s->u, "no-enosys", missing == NULL,
               "the compiler asked this kernel for a system call it does not have: %s",
               net_copy_line(missing, line, sizeof(line)));
    free(text);
}

/*
 * The rebuild case's project: two sources and a header they share, so that
 * touching the header must rebuild both objects and touching one source
 * exactly one. The makefile names a compiler that does not exist, and every
 * make but one overrides it on the command line: a make that ignored the
 * override would fail outright, where an override that named the built-in
 * default would prove nothing.
 *
 * make rebuilds a target whose time is not later than a prerequisite's, and
 * a fast machine can write a file and build from it within one tick of the
 * clock. So `age`, before a line that must find the tree up to date or a
 * change newer than what was built from it, gives the files times in the
 * past in the order of the build: the sources, then the objects, then the
 * program. A change made after it has the present time.
 */
static const char age_line[] = "age() { n=$(date +%s); busybox touch -c -d @$((n-4)) demo.h lib.c "
                               "main.c Makefile; busybox touch -c -d @$((n-2)) lib.o main.o; "
                               "busybox touch -c -d @$((n-1)) prog; }";
static const char lib_line[] =
    "printf '#include \"demo.h\"\\nint scale(int x){return 2*x;}\\n' > lib.c";
static const char main_line[] = "printf '#include <stdio.h>\\n#include \"demo.h\"\\nint main(void)"
                                "{printf(\"ANSWER %%d\\\\n\", scale(21));return 0;}\\n' > main.c";
static const char makefile_line[] = "printf 'CC=/no/such/compiler\\nprog: main.o lib.o\\n\\t$(CC) "
                                    "$(LDFLAGS) -o prog main.o lib.o\\nmain.o: main.c demo.h\\n"
                                    "lib.o: lib.c demo.h\\nclean:\\n\\trm -f prog main.o lib.o\\n"
                                    ".PHONY: clean\\n' > Makefile";
static const char idle1_line[] = "age; make CC=tcc > /tmp/idle1.out 2>&1; echo IDLE1\"\"RC=$? "
                                 "TCC=$(grep -c tcc /tmp/idle1.out) UP=$(grep -c 'up to date' "
                                 "/tmp/idle1.out)";
static const char change_line[] =
    "age; printf '#include \"demo.h\"\\nint scale(int x){return 3*x;}\\n' "
    "> lib.c";
static const char one_line[] = "make CC=tcc > /tmp/one.out 2>&1; echo ONE\"\"RC=$? LIB=$(grep -c "
                               "'c lib.c' /tmp/one.out) MAIN=$(grep -c 'c main.c' /tmp/one.out) "
                               "LINK=$(grep -c 'o prog' /tmp/one.out)";
static const char idle2_line[] =
    "make CC=tcc > /tmp/idle2.out 2>&1; echo IDLE2\"\"RC=$? TCC=$(grep "
    "-c tcc /tmp/idle2.out) UP=$(grep -c 'up to date' /tmp/idle2.out)";
static const char header_line[] =
    "make CC=tcc > /tmp/hdr.out 2>&1; echo HDR\"\"RC=$? LIB=$(grep -c "
    "'c lib.c' /tmp/hdr.out) MAIN=$(grep -c 'c main.c' /tmp/hdr.out)";
static const char override_line[] =
    "age; touch demo.h; make > /tmp/over.out 2>&1; echo NO\"\"OVER=$? "
    "SAW=$(grep -q 'no/such/compiler' /tmp/over.out && echo yes || "
    "echo no)";
static const char clean_line[] = "> clean; make CC=tcc clean > /tmp/clean.out 2>&1; echo "
                                 "CLEAN\"\"DONE=$? LEFT=$(ls prog main.o 2>/dev/null | wc -l)";
static const char util_line[] = "mkdir -p /tmp/u && cd /tmp/u && cp /src/utils/cat.c . && printf "
                                "'cat: cat.o\\n\\t$(CC) $(LDFLAGS) -o cat cat.o\\n' > Makefile && "
                                "make CC=tcc > /tmp/u.out 2>&1; echo UTIL\"\"RC=$?";
static const char same_util_line[] =
    "printf 'a line this machine built\\n' > /tmp/u/in.txt; /tmp/u/cat "
    "/tmp/u/in.txt > /tmp/u/mine; busybox cat /tmp/u/in.txt > "
    "/tmp/u/theirs; cmp /tmp/u/mine /tmp/u/theirs && echo "
    "UTIL\"\"SAME";
static const char stamp_line[] =
    "cd /tmp/demo && make CC=tcc > /dev/null 2>&1; age; busybox stat -c "
    "%Y lib.o > lib.o.when; busybox stat -c 'TIMES %Y %n' Makefile "
    "demo.h lib.c main.c lib.o main.o prog; echo STAMP\"\"ED=$(cat "
    "lib.o.when)";
static const char kept_line[] = "cd /tmp/demo; echo KEP\"\"T=$(busybox stat -c %Y lib.o) "
                                "WANTED=$(cat lib.o.when)";
static const char reboot_idle_line[] = "busybox stat -c 'TIMES %Y %n' Makefile demo.h lib.c main.c "
                                       "lib.o main.o prog; make CC=tcc > /tmp/r1.out 2>&1; echo "
                                       "REBOOT\"\"IDLE=$? TCC=$(grep -c tcc /tmp/r1.out); sed "
                                       "'s/^/REMADE: /' /tmp/r1.out";
static const char reboot_change_line[] = "printf '#include \"demo.h\"\\nint scale(int x){return "
                                         "4*x;}\\n' > lib.c";
static const char reboot_make_line[] = "make CC=tcc > /tmp/r2.out 2>&1; echo REBOOT\"\"RC=$? "
                                       "LIB=$(grep -c 'c lib.c' /tmp/r2.out)";

/**
 * Run case `rebuild`, as the test account, since a build tool that only root
 * can use is not the one a student needs. Every judgement is of one line's
 * own output: a count over the whole console would count the earlier lines'
 * output too.
 *
 * The first make after the build is a control: it passes on a machine with
 * no clock at all, and tells a failure below from a make that never worked.
 * After one source changes, exactly one object recompiles, the program
 * relinks once, and the program that runs is the new one, which a make that
 * rebuilt the object and never relinked would fail. The make straight after
 * that rebuild is the step that discriminates: an object that nothing
 * stamped stays older than its source, and the same rebuild runs on every
 * make. The make without the override must fail and name the compiler it
 * could not find, which gives the override on every other line its meaning.
 *
 * Then the object's time is recorded, the disk rebooted, and two things are
 * asked. The times must survive exactly, which a mount that gave every file
 * one boot-time value would fail. And the clock must not have gone back: a
 * machine whose time restarts at a boot makes a source changed after it
 * older than the object built before it, and make does nothing. The first
 * boot leaves the object's time two seconds in the past, which leaves room
 * for the part of a second the clock drops at a boot, when it starts from
 * the real-time clock's whole seconds.
 */
static void rebuild(struct shell *s)
{
    char want[80];
    long stamp, failed;

    net_step(&s->u, "boot");
    login(s, 1, "login");

    net_step(&s->u, "build");
    type(s, "cd /tmp && rm -rf demo && mkdir demo && cd demo && echo SETUP\"\"DONE");
    need(s, "SETUPDONE", "the project's directory was not made");
    type(s, age_line);
    type(s, "printf 'int scale(int);\\n' > demo.h");
    type(s, lib_line);
    type(s, main_line);
    type(s, makefile_line);
    type(s, "make CC=tcc > /tmp/first.out 2>&1; echo FIRST\"\"RC=$?");
    has(s, "first-build", "FIRSTRC=0", "make did not build the program from scratch");
    type(s, "./prog > /tmp/first.run 2>&1; echo FIRST\"\"RAN=$(cat /tmp/first.run)");
    has(s, "first-ran", "FIRSTRAN=ANSWER 42",
        "the program make built did not run, or printed the wrong thing");

    net_step(&s->u, "rebuild");
    type(s, idle1_line);
    has(s, "idle-1", "IDLE1RC=0 TCC=0 UP=1",
        "a second make with nothing changed did not report the program up to date");
    type(s, change_line);
    type(s, one_line);
    has(s, "one-source", "ONERC=0 LIB=1 MAIN=0 LINK=1",
        "changing one source did not recompile exactly it and relink once");
    type(s, "./prog > /tmp/one.run 2>&1; echo ONE\"\"RAN=$(cat /tmp/one.run)");
    has(s, "one-ran", "ONERAN=ANSWER 63", "the program did not change after its source did");
    type(s, idle2_line);
    has(s, "idle-2", "IDLE2RC=0 TCC=0 UP=1",
        "a make immediately after a rebuild ran the compiler again: nothing stamped the object");
    type(s, "age; touch demo.h");
    type(s, header_line);
    has(s, "header", "HDRRC=0 LIB=1 MAIN=1",
        "touching the shared header did not rebuild both objects");
    type(s, override_line);
    failed = number(s, "NOOVER=");
    net_assert(&s->u, "no-override", failed > 0,
               "a make whose makefile names a compiler that does not exist gave status %ld",
               failed);
    has(s, "named-compiler", "SAW=yes",
        "the make without the override did not name the compiler it could not find");
    type(s, clean_line);
    has(s, "phony-clean", "CLEANDONE=0 LEFT=0",
        ".PHONY clean did not run, or left its targets, with a file named clean present");

    net_step(&s->u, "util");
    type(s, util_line);
    has(s, "util-built", "UTILRC=0", "make could not build this project's own cat");
    type(s, same_util_line);
    has(s, "util-same", "UTILSAME", "the cat make built does not agree with the shipped one");

    net_step(&s->u, "stamp");
    type(s, stamp_line);
    stamp = number(s, "STAMPED=");
    net_assert(&s->u, "stamped", stamp > 0,
               "the object's recorded time is missing or the epoch: %ld", stamp);
    finish(s, "first-done");

    net_step(&s->u, "reboot");
    login(s, 2, "relogin");

    net_step(&s->u, "after");
    type(s, kept_line);
    snprintf(want, sizeof(want), "KEPT=%ld WANTED=%ld", stamp, stamp);
    net_assert(&s->u, "kept", stamp > 0 && s->out != NULL && strstr(s->out, want) != NULL,
               "the object's time did not survive the reboot exactly (wanted %ld)", stamp);
    type(s, reboot_idle_line);
    has(s, "reboot-idle", "REBOOTIDLE=0 TCC=0",
        "a make after a reboot rebuilt something with nothing touched");
    type(s, reboot_change_line);
    type(s, reboot_make_line);
    has(s, "reboot-change", "REBOOTRC=0 LIB=1",
        "a source changed after a reboot did not rebuild: the clock restarted");
    type(s, "./prog > /tmp/r2.run 2>&1; echo REBOOT\"\"RAN=$(cat /tmp/r2.run)");
    has(s, "reboot-ran", "REBOOTRAN=ANSWER 84",
        "the program rebuilt after the reboot is not the new one");
    finish(s, "second-done");
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    struct shell s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    net_secret(&s.u, "test-password", s.password, sizeof(s.password));
    if (strcmp(id, "machine.tcc.compile") == 0)
        compile(&s);
    else if (strcmp(id, "machine.make.rebuild") == 0)
        rebuild(&s);
    else
        return 1;
    free(s.out);
    return net_end(&s.u);
}
