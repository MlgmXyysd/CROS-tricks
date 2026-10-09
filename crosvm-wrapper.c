/* crosvm-wrapper.c — freestanding "disk order" shim for the borealis VM.
 *
 * Installed as /usr/bin/crosvm, with the stock binary moved aside to
 * /usr/bin/crosvm.real. Every exec is forwarded to the real crosvm; only for
 * the borealis VM is the argument list rewritten on the way through.
 *
 * Why: concierge attaches the DLC rootfs first (guest /dev/vda) and the
 * stateful disk second (guest /dev/vdb), but the guest image expects the
 * stateful disk on /dev/vda and the rootfs on /dev/vdb. crosvm hands out
 * /dev/vdX in the order its --block options are given, so moving the disk
 * that carries root=true to the END of the --block list swaps the two (the
 * relative order of the remaining disks is preserved) and crosvm then emits
 * root=/dev/vdb, which is what the image wants.
 *
 * Freestanding: no libc, no headers, no crt — custom _start + raw syscalls.
 * x86_64 only (there is no aarch64 borealis-dlc redist to fix).
 *
 * It has to be a STATIC binary: concierge puts
 * LD_PRELOAD=/lib64/libminijailpreload.so on its first exec'd child, and the
 * minijail interposer jails whatever gets exec'd first — a dynamic
 * interpreter (python, dash) is jailed as if it were crosvm and dies
 * instantly, while a static binary ignores LD_PRELOAD, runs clean, and its
 * exec of the real crosvm is intercepted exactly like the original direct
 * exec was.
 *
 * Build (native x86_64):
 *   gcc -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
 *       -nostdlib -c -o wrap.o crosvm-wrapper.c
 *   ld -static -o crosvm-wrapper wrap.o
 *
 * Debug: WRAP_DEBUG=<anything> in the environment makes it print its argv to
 * stderr and exit 0 without exec'ing.
 */
typedef long i64;
typedef unsigned long u64;

#define SYS_write 1
#define SYS_execve 59
#define SYS_exit_group 231

/* syscall(nr, a, b, c) — x86_64 ABI: nr in rax, args in rdi/rsi/rdx */
static i64 sys3(i64 n, i64 a, i64 b, i64 c) {
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Entry point instead of crt0: the kernel leaves argc at (%rsp) with no return
 * address above it, so the original stack pointer is handed to c_main. %rsp is
 * re-aligned for the call (the ABI wants 16 bytes at the call site). _start is
 * never reached again — exit_group ends the process with c_main's status. */
__asm__(".global _start\n"
        "_start:\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call c_main\n"
        "  mov %eax, %edi\n"
        "  mov $231, %eax\n"
        "  syscall\n");

/* full-string comparison (1 = equal) */
static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* substring test (1 = n occurs anywhere in h) */
static int s_has(const char *h, const char *n) {
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* string length */
static u64 s_len(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

/* write(2, ...) — diagnostics go to stderr so crosvm's own stdout stays clean */
static void out(const char *s) {
    sys3(SYS_write, 2, (i64)s, (i64)s_len(s));
}

long c_main(long *sp) {
    /* the raw stack we were started on: sp[0] = argc, argv[argc] = NULL, envp */
    long argc = sp[0];
    char **argv = (char **)&sp[1];
    char **envp = argv + argc + 1;
    long i;

    /* the borealis crosvm command line carries the base64 tag "Ym9yZWFsaXM=";
     * every other VM (termina, ...) shares this binary and must be passed
     * through untouched */
    int is_borealis = 0;
    for (i = 1; i < argc; i++)
        if (s_has(argv[i], "Ym9yZWFsaXM=")) { /* borealis */
            is_borealis = 1;
            break;
        }

    if (is_borealis) {
        long bpos[8];   /* argv slot of each --block value; 8 is far more than
                         * borealis passes (2-3), anything beyond is left as is */
        int nb = 0;
        for (i = 1; i + 1 < argc && nb < 8; i++)
            if (s_eq(argv[i], "--block")) bpos[nb++] = i + 1;
        if (nb >= 2) {
            char *tmp[8];
            int w = 0, k;
            for (k = 0; k < nb; k++) tmp[k] = argv[bpos[k]];
            /* two stable passes written back into the same slots: everything
             * except root=true first, then the root=true disks — so the rootfs
             * attaches last, the stateful disk takes /dev/vda and crosvm emits
             * root=/dev/vdb. Already-last is a no-op, so this is idempotent. */
            for (k = 0; k < nb; k++)
                if (!s_has(tmp[k], "root=true")) argv[bpos[w++]] = tmp[k];
            for (k = 0; k < nb; k++)
                if (s_has(tmp[k], "root=true")) argv[bpos[w++]] = tmp[k];
        }
    }

    {   /* WRAP_DEBUG=<anything> → print argv to stderr and exit 0 without
         * exec'ing, to see what concierge actually passes */
        char **e;
        for (e = envp; *e; e++) {
            if (s_has(*e, "WRAP_DEBUG=")) {
                for (i = 0; i < argc; i++) { out(argv[i]); out("\n"); }
                return 0;
            }
        }
    }

    /* become the real crosvm (argv[0] rewritten so the process is identifiable
     * as the stock binary); execve only returns when it failed — exit 127, the
     * same status a shell reports for a command it could not run */
    argv[0] = (char *)"/usr/bin/crosvm.real";
    sys3(SYS_execve, (i64)"/usr/bin/crosvm.real", (i64)argv, (i64)envp);
    out("crosvm-wrap: execve failed\n");
    return 127;
}
