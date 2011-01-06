/* system/debuggerd/debuggerd.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/exec_elf.h>
#include <sys/stat.h>

#include <cutils/sockets.h>
#include <cutils/logd.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>

#include <linux/input.h>

#include <private/android_filesystem_config.h>

#include "utility.h"

#ifdef WITH_VFP
#ifdef WITH_VFP_D32
#define NUM_VFP_REGS 32
#else
#define NUM_VFP_REGS 16
#endif
#endif

/* Main entry point to get the backtrace from the crashing process */
extern int unwind_backtrace_with_ptrace(int tfd, pid_t pid, mapinfo *map,
                                        unsigned int sp_list[],
                                        int *frame0_pc_sane,
                                        bool at_fault);

static int logsocket = -1;

#define ANDROID_LOG_INFO 4

/* Log information onto the tombstone */
void _LOG(int tfd, bool in_tombstone_only, const char *fmt, ...)
{
    char buf[128];

    va_list ap;
    va_start(ap, fmt);

    if (tfd >= 0) {
        int len;
        vsnprintf(buf, sizeof(buf), fmt, ap);
        len = strlen(buf);
        if(tfd >= 0) write(tfd, buf, len);
    }

    if (!in_tombstone_only)
        __android_log_vprint(ANDROID_LOG_INFO, "DEBUG", fmt, ap);
}

#define LOG(fmt...) _LOG(-1, 0, fmt)
#if 0
#define XLOG(fmt...) _LOG(-1, 0, fmt)
#else
#define XLOG(fmt...) do {} while(0)
#endif

// 6f000000-6f01e000 rwxp 00000000 00:0c 16389419   /system/lib/libcomposer.so
// 012345678901234567890123456789012345678901234567890123456789
// 0         1         2         3         4         5

mapinfo *parse_maps_line(char *line)
{
    mapinfo *mi;
    int len = strlen(line);

    if(len < 1) return 0;
    line[--len] = 0;

    if(len < 50) return 0;
    if(line[20] != 'x') return 0;

    mi = malloc(sizeof(mapinfo) + (len - 47));
    if(mi == 0) return 0;

    mi->start = strtoul(line, 0, 16);
    mi->end = strtoul(line + 9, 0, 16);
    /* To be filled in parse_exidx_info if the mapped section starts with
     * elf_header
     */
    mi->exidx_start = mi->exidx_end = 0;
    mi->next = 0;
    strcpy(mi->name, line + 49);

    return mi;
}

void dump_build_info(int tfd)
{
    char fingerprint[PROPERTY_VALUE_MAX];

    property_get("ro.build.fingerprint", fingerprint, "unknown");

    _LOG(tfd, false, "Build fingerprint: '%s'\n", fingerprint);
}


void dump_stack_and_code(int tfd, int pid, mapinfo *map,
                         int unwind_depth, unsigned int sp_list[],
                         bool at_fault)
{
    unsigned int sp, pc, p, end, data;
    struct pt_regs r;
    int sp_depth;
    bool only_in_tombstone = !at_fault;
    char code_buffer[80];

    if(ptrace(PTRACE_GETREGS, pid, 0, &r)) return;
    sp = r.ARM_sp;
    pc = r.ARM_pc;

    _LOG(tfd, only_in_tombstone, "\ncode around pc:\n");

    end = p = pc & ~3;
    p -= 32;
    end += 32;

    /* Dump the code around PC as:
     *  addr       contents
     *  00008d34   fffffcd0 4c0eb530 b0934a0e 1c05447c
     *  00008d44   f7ff18a0 490ced94 68035860 d0012b00
     */
    while (p <= end) {
        int i;

        sprintf(code_buffer, "%08x ", p);
        for (i = 0; i < 4; i++) {
            data = ptrace(PTRACE_PEEKTEXT, pid, (void*)p, NULL);
            sprintf(code_buffer + strlen(code_buffer), "%08x ", data);
            p += 4;
        }
        _LOG(tfd, only_in_tombstone, "%s\n", code_buffer);
    }

    if ((unsigned) r.ARM_lr != pc) {
        _LOG(tfd, only_in_tombstone, "\ncode around lr:\n");

        end = p = r.ARM_lr & ~3;
        p -= 32;
        end += 32;

        /* Dump the code around LR as:
         *  addr       contents
         *  00008d34   fffffcd0 4c0eb530 b0934a0e 1c05447c
         *  00008d44   f7ff18a0 490ced94 68035860 d0012b00
         */
        while (p <= end) {
            int i;

            sprintf(code_buffer, "%08x ", p);
            for (i = 0; i < 4; i++) {
                data = ptrace(PTRACE_PEEKTEXT, pid, (void*)p, NULL);
                sprintf(code_buffer + strlen(code_buffer), "%08x ", data);
                p += 4;
            }
            _LOG(tfd, only_in_tombstone, "%s\n", code_buffer);
        }
    }

    p = sp - 64;
    p &= ~3;
    if (unwind_depth != 0) {
        if (unwind_depth < STACK_CONTENT_DEPTH) {
            end = sp_list[unwind_depth-1];
        }
        else {
            end = sp_list[STACK_CONTENT_DEPTH-1];
        }
    }
    else {
        end = sp | 0x000000ff;
        end += 0xff;
    }

    _LOG(tfd, only_in_tombstone, "\nstack:\n");

    /* If the crash is due to PC == 0, there will be two frames that
     * have identical SP value.
     */
    if (sp_list[0] == sp_list[1]) {
        sp_depth = 1;
    }
    else {
        sp_depth = 0;
    }

    while (p <= end) {
         char *prompt;
         char level[16];
         data = ptrace(PTRACE_PEEKTEXT, pid, (void*)p, NULL);
         if (p == sp_list[sp_depth]) {
             sprintf(level, "#%02d", sp_depth++);
             prompt = level;
         }
         else {
             prompt = "   ";
         }

         /* Print the stack content in the log for the first 3 frames. For the
          * rest only print them in the tombstone file.
          */
         _LOG(tfd, (sp_depth > 2) || only_in_tombstone,
              "%s %08x  %08x  %s\n", prompt, p, data,
              map_to_name(map, data, ""));
         p += 4;
    }
    /* print another 64-byte of stack data after the last frame */

    end = p+64;
    while (p <= end) {
         data = ptrace(PTRACE_PEEKTEXT, pid, (void*)p, NULL);
         _LOG(tfd, (sp_depth > 2) || only_in_tombstone,
              "    %08x  %08x  %s\n", p, data,
              map_to_name(map, data, ""));
         p += 4;
    }
}

void dump_pc_and_lr(int tfd, int pid, mapinfo *map, int unwound_level,
                    bool at_fault)
{
    struct pt_regs r;

    if(ptrace(PTRACE_GETREGS, pid, 0, &r)) {
        _LOG(tfd, !at_fault, "tid %d not responding!\n", pid);
        return;
    }

    if (unwound_level == 0) {
        _LOG(tfd, !at_fault, "         #%02d  pc %08x  %s\n", 0, r.ARM_pc,
             map_to_name(map, r.ARM_pc, "<unknown>"));
    }
    _LOG(tfd, !at_fault, "         #%02d  lr %08x  %s\n", 1, r.ARM_lr,
            map_to_name(map, r.ARM_lr, "<unknown>"));
}

void dump_registers(int tfd, int pid, bool at_fault)
{
    struct pt_regs r;
    bool only_in_tombstone = !at_fault;

    if(ptrace(PTRACE_GETREGS, pid, 0, &r)) {
        _LOG(tfd, only_in_tombstone,
             "cannot get registers: %s\n", strerror(errno));
        return;
    }

    _LOG(tfd, only_in_tombstone, " r0 %08x  r1 %08x  r2 %08x  r3 %08x\n",
         r.ARM_r0, r.ARM_r1, r.ARM_r2, r.ARM_r3);
    _LOG(tfd, only_in_tombstone, " r4 %08x  r5 %08x  r6 %08x  r7 %08x\n",
         r.ARM_r4, r.ARM_r5, r.ARM_r6, r.ARM_r7);
    _LOG(tfd, only_in_tombstone, " r8 %08x  r9 %08x  10 %08x  fp %08x\n",
         r.ARM_r8, r.ARM_r9, r.ARM_r10, r.ARM_fp);
    _LOG(tfd, only_in_tombstone,
         " ip %08x  sp %08x  lr %08x  pc %08x  cpsr %08x\n",
         r.ARM_ip, r.ARM_sp, r.ARM_lr, r.ARM_pc, r.ARM_cpsr);

#ifdef WITH_VFP
    struct user_vfp vfp_regs;
    int i;

    if(ptrace(PTRACE_GETVFPREGS, pid, 0, &vfp_regs)) {
        _LOG(tfd, only_in_tombstone,
             "cannot get registers: %s\n", strerror(errno));
        return;
    }

    for (i = 0; i < NUM_VFP_REGS; i += 2) {
        _LOG(tfd, only_in_tombstone,
             " d%-2d %016llx  d%-2d %016llx\n",
              i, vfp_regs.fpregs[i], i+1, vfp_regs.fpregs[i+1]);
    }
    _LOG(tfd, only_in_tombstone, " scr %08lx\n\n", vfp_regs.fpscr);
#endif
}

const char *get_signame(int sig)
{
    switch(sig) {
    case SIGILL:     return "SIGILL";
    case SIGABRT:    return "SIGABRT";
    case SIGBUS:     return "SIGBUS";
    case SIGFPE:     return "SIGFPE";
    case SIGSEGV:    return "SIGSEGV";
    case SIGSTKFLT:  return "SIGSTKFLT";
    default:         return "?";
    }
}

void dump_fault_addr(int tfd, int pid, int sig)
{
    siginfo_t si;

    memset(&si, 0, sizeof(si));
    if(ptrace(PTRACE_GETSIGINFO, pid, 0, &si)){
        _LOG(tfd, false, "cannot get siginfo: %s\n", strerror(errno));
    } else {
        _LOG(tfd, false, "signal %d (%s), fault addr %08x\n",
            sig, get_signame(sig), si.si_addr);
    }
}

void dump_crash_banner(int tfd, unsigned pid, unsigned tid, int sig)
{
    char data[1024];
    char *x = 0;
    FILE *fp;

    sprintf(data, "/proc/%d/cmdline", pid);
    fp = fopen(data, "r");
    if(fp) {
        x = fgets(data, 1024, fp);
        fclose(fp);
    }

    _LOG(tfd, false,
         "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
    dump_build_info(tfd);
    _LOG(tfd, false, "pid: %d, tid: %d  >>> %s <<<\n",
         pid, tid, x ? x : "UNKNOWN");

    if(sig) dump_fault_addr(tfd, tid, sig);
}

static void parse_exidx_info(mapinfo *milist, pid_t pid)
{
    mapinfo *mi;
    for (mi = milist; mi != NULL; mi = mi->next) {
        Elf32_Ehdr ehdr;

        memset(&ehdr, 0, sizeof(Elf32_Ehdr));
        /* Read in sizeof(Elf32_Ehdr) worth of data from the beginning of
         * mapped section.
         */
        get_remote_struct(pid, (void *) (mi->start), &ehdr,
                          sizeof(Elf32_Ehdr));
        /* Check if it has the matching magic words */
        if (IS_ELF(ehdr)) {
            Elf32_Phdr phdr;
            Elf32_Phdr *ptr;
            int i;

            ptr = (Elf32_Phdr *) (mi->start + ehdr.e_phoff);
            for (i = 0; i < ehdr.e_phnum; i++) {
                /* Parse the program header */
                get_remote_struct(pid, (char *) ptr+i, &phdr,
                                  sizeof(Elf32_Phdr));
                /* Found a EXIDX segment? */
                if (phdr.p_type == PT_ARM_EXIDX) {
                    mi->exidx_start = mi->start + phdr.p_offset;
                    mi->exidx_end = mi->exidx_start + phdr.p_filesz;
                    break;
                }
            }
        }
    }
}

void dump_crash_report(int tfd, unsigned pid, unsigned tid, bool at_fault)
{
    char data[1024];
    FILE *fp;
    mapinfo *milist = 0;
    unsigned int sp_list[STACK_CONTENT_DEPTH];
    int stack_depth;
    int frame0_pc_sane = 1;

    if (!at_fault) {
        _LOG(tfd, true,
         "--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---\n");
        _LOG(tfd, true, "pid: %d, tid: %d\n", pid, tid);
    }

    dump_registers(tfd, tid, at_fault);

    /* Clear stack pointer records */
    memset(sp_list, 0, sizeof(sp_list));

    sprintf(data, "/proc/%d/maps", pid);
    fp = fopen(data, "r");
    if(fp) {
        while(fgets(data, 1024, fp)) {
            mapinfo *mi = parse_maps_line(data);
            if(mi) {
                mi->next = milist;
                milist = mi;
            }
        }
        fclose(fp);
    }

    parse_exidx_info(milist, tid);

    /* If stack unwinder fails, use the default solution to dump the stack
     * content.
     */
    stack_depth = unwind_backtrace_with_ptrace(tfd, tid, milist, sp_list,
                                               &frame0_pc_sane, at_fault);

    /* The stack unwinder should at least unwind two levels of stack. If less
     * level is seen we make sure at lease pc and lr are dumped.
     */
    if (stack_depth < 2) {
        dump_pc_and_lr(tfd, tid, milist, stack_depth, at_fault);
    }

    dump_stack_and_code(tfd, tid, milist, stack_depth, sp_list, at_fault);

    while(milist) {
        mapinfo *next = milist->next;
        free(milist);
        milist = next;
    }
}

#define MAX_TOMBSTONES	10

#define typecheck(x,y) {    \
    typeof(x) __dummy1;     \
    typeof(y) __dummy2;     \
    (void)(&__dummy1 == &__dummy2); }

#define TOMBSTONE_DIR	"/data/tombstones"

/*
 * find_and_open_tombstone - find an available tombstone slot, if any, of the
 * form tombstone_XX where XX is 00 to MAX_TOMBSTONES-1, inclusive. If no
 * file is available, we reuse the least-recently-modified file.
 */
static int find_and_open_tombstone(void)
{
    unsigned long mtime = ULONG_MAX;
    struct stat sb;
    char path[128];
    int fd, i, oldest = 0;

    /*
     * XXX: Our stat.st_mtime isn't time_t. If it changes, as it probably ought
     * to, our logic breaks. This check will generate a warning if that happens.
     */
    typecheck(mtime, sb.st_mtime);

    /*
     * In a single wolf-like pass, find an available slot and, in case none
     * exist, find and record the least-recently-modified file.
     */
    for (i = 0; i < MAX_TOMBSTONES; i++) {
        snprintf(path, sizeof(path), TOMBSTONE_DIR"/tombstone_%02d", i);

        if (!stat(path, &sb)) {
            if (sb.st_mtime < mtime) {
                oldest = i;
                mtime = sb.st_mtime;
            }
            continue;
        }
        if (errno != ENOENT)
            continue;

        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0)
            continue;	/* raced ? */

        fchown(fd, AID_SYSTEM, AID_SYSTEM);
        return fd;
    }

    /* we didn't find an available file, so we clobber the oldest one */
    snprintf(path, sizeof(path), TOMBSTONE_DIR"/tombstone_%02d", oldest);
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    fchown(fd, AID_SYSTEM, AID_SYSTEM);

    return fd;
}

/* Return true if some thread is not detached cleanly */
static bool dump_sibling_thread_report(int tfd, unsigned pid, unsigned tid)
{
    char task_path[1024];

    sprintf(task_path, "/proc/%d/task", pid);
    DIR *d;
    struct dirent *de;
    int need_cleanup = 0;

    d = opendir(task_path);
    /* Bail early if cannot open the task directory */
    if (d == NULL) {
        XLOG("Cannot open /proc/%d/task\n", pid);
        return false;
    }
    while ((de = readdir(d)) != NULL) {
        unsigned new_tid;
        /* Ignore "." and ".." */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        new_tid = atoi(de->d_name);
        /* The main thread at fault has been handled individually */
        if (new_tid == tid)
            continue;

        /* Skip this thread if cannot ptrace it */
        if (ptrace(PTRACE_ATTACH, new_tid, 0, 0) < 0)
            continue;

        dump_crash_report(tfd, pid, new_tid, false);
        need_cleanup |= ptrace(PTRACE_DETACH, new_tid, 0, 0);
    }
    closedir(d);
    return need_cleanup != 0;
}

/* Return true if some thread is not detached cleanly */
static bool engrave_tombstone(unsigned pid, unsigned tid, int debug_uid,
                              int signal)
{
    int fd;
    bool need_cleanup = false;

    mkdir(TOMBSTONE_DIR, 0755);
    chown(TOMBSTONE_DIR, AID_SYSTEM, AID_SYSTEM);

    fd = find_and_open_tombstone();
    if (fd < 0)
        return need_cleanup;

    dump_crash_banner(fd, pid, tid, signal);
    dump_crash_report(fd, pid, tid, true);
    /*
     * If the user has requested to attach gdb, don't collect the per-thread
     * information as it increases the chance to lose track of the process.
     */
    if ((signed)pid > debug_uid) {
        need_cleanup = dump_sibling_thread_report(fd, pid, tid);
    }

    close(fd);
    return need_cleanup;
}

static int
write_string(const char* file, const char* string)
{
    int len;
    int fd;
    ssize_t amt;
    fd = open(file, O_RDWR);
    len = strlen(string);
    if (fd < 0)
        return -errno;
    amt = write(fd, string, len);
    close(fd);
    return amt >= 0 ? 0 : -errno;
}

static
void init_debug_led(void)
{
    // trout leds
    write_string("/sys/class/leds/red/brightness", "0");
    write_string("/sys/class/leds/green/brightness", "0");
    write_string("/sys/class/leds/blue/brightness", "0");
    write_string("/sys/class/leds/red/device/blink", "0");
    // sardine leds
    write_string("/sys/class/leds/left/cadence", "0,0");
}

static
void enable_debug_led(void)
{
    // trout leds
    write_string("/sys/class/leds/red/brightness", "255");
    // sardine leds
    write_string("/sys/class/leds/left/cadence", "1,0");
}

static
void disable_debug_led(void)
{
    // trout leds
    write_string("/sys/class/leds/red/brightness", "0");
    // sardine leds
    write_string("/sys/class/leds/left/cadence", "0,0");
}

extern int init_getevent();
extern void uninit_getevent();
extern int get_event(struct input_event* event, int timeout);

static void wait_for_user_action(unsigned tid, struct ucred* cr)
{
    (void)tid;
    /* First log a helpful message */
    LOG(    "********************************************************\n"
            "* Process %d has been suspended while crashing.  To\n"
            "* attach gdbserver for a gdb connection on port 5039:\n"
            "*\n"
            "*     adb shell gdbserver :5039 --attach %d &\n"
            "*\n"
            "* Press HOME key to let the process continue crashing.\n"
            "********************************************************\n",
            cr->pid, cr->pid);

    /* wait for HOME key (TODO: something useful for devices w/o HOME key) */
    if (init_getevent() == 0) {
        int ms = 1200 / 10;
        int dit = 1;
        int dah = 3*dit;
        int _       = -dit;
        int ___     = 3*_;
        int _______ = 7*_;
        const signed char codes[] = {
           dit,_,dit,_,dit,___,dah,_,dah,_,dah,___,dit,_,dit,_,dit,_______
        };
        size_t s = 0;
        struct input_event e;
        int home = 0;
        init_debug_led();
        enable_debug_led();
        do {
            int timeout = abs((int)(codes[s])) * ms;
            int res = get_event(&e, timeout);
            if (res == 0) {
                if (e.type==EV_KEY && e.code==KEY_HOME && e.value==0)
                    home = 1;
            } else if (res == 1) {
                if (++s >= sizeof(codes)/sizeof(*codes))
                    s = 0;
                if (codes[s] > 0) {
                    enable_debug_led();
                } else {
                    disable_debug_led();
                }
            }
        } while (!home);
        uninit_getevent();
    }

    /* don't forget to turn debug led off */
    disable_debug_led();

    /* close filedescriptor */
    LOG("debuggerd resuming process %d", cr->pid);
 }

static void handle_crashing_process(int fd)
{
    char buf[64];
    struct stat s;
    unsigned tid;
    struct ucred cr;
    int n, len, status;
    int tid_attach_status = -1;
    unsigned retry = 30;
    bool need_cleanup = false;

    char value[PROPERTY_VALUE_MAX];
    property_get("debug.db.uid", value, "-1");
    int debug_uid = atoi(value);

    XLOG("handle_crashing_process(%d)\n", fd);

    len = sizeof(cr);
    n = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len);
    if(n != 0) {
        LOG("cannot get credentials\n");
        goto done;
    }

    XLOG("reading tid\n");
    fcntl(fd, F_SETFL, O_NONBLOCK);
    while((n = read(fd, &tid, sizeof(unsigned))) != sizeof(unsigned)) {
        if(errno == EINTR) continue;
        if(errno == EWOULDBLOCK) {
            if(retry-- > 0) {
                usleep(100 * 1000);
                continue;
            }
            LOG("timed out reading tid\n");
            goto done;
        }
        LOG("read failure? %s\n", strerror(errno));
        goto done;
    }

    sprintf(buf,"/proc/%d/task/%d", cr.pid, tid);
    if(stat(buf, &s)) {
        LOG("tid %d does not exist in pid %d. ignoring debug request\n",
            tid, cr.pid);
        close(fd);
        return;
    }

    XLOG("BOOM: pid=%d uid=%d gid=%d tid=%d\n", cr.pid, cr.uid, cr.gid, tid);

    tid_attach_status = ptrace(PTRACE_ATTACH, tid, 0, 0);
    if(tid_attach_status < 0) {
        LOG("ptrace attach failed: %s\n", strerror(errno));
        goto done;
    }

    close(fd);
    fd = -1;

    for(;;) {
        n = waitpid(tid, &status, __WALL);

        if(n < 0) {
            if(errno == EAGAIN) continue;
            LOG("waitpid failed: %s\n", strerror(errno));
            goto done;
        }

        XLOG("waitpid: n=%d status=%08x\n", n, status);

        if(WIFSTOPPED(status)){
            n = WSTOPSIG(status);
            switch(n) {
            case SIGSTOP:
                XLOG("stopped -- continuing\n");
                n = ptrace(PTRACE_CONT, tid, 0, 0);
                if(n) {
                    LOG("ptrace failed: %s\n", strerror(errno));
                    goto done;
                }
                continue;

            case SIGILL:
            case SIGABRT:
            case SIGBUS:
            case SIGFPE:
            case SIGSEGV:
            case SIGSTKFLT: {
                XLOG("stopped -- fatal signal\n");
                need_cleanup = engrave_tombstone(cr.pid, tid, debug_uid, n);
                kill(tid, SIGSTOP);
                goto done;
            }

            default:
                XLOG("stopped -- unexpected signal\n");
                goto done;
            }
        } else {
            XLOG("unexpected waitpid response\n");
            goto done;
        }
    }

done:
    XLOG("detaching\n");

    /* stop the process so we can debug */
    kill(cr.pid, SIGSTOP);

    /*
     * If a thread has been attached by ptrace, make sure it is detached
     * successfully otherwise we will get a zombie.
     */
    if (tid_attach_status == 0) {
        int detach_status;
        /* detach so we can attach gdbserver */
        detach_status = ptrace(PTRACE_DETACH, tid, 0, 0);
        need_cleanup |= (detach_status != 0);
    }

    /*
     * if debug.db.uid is set, its value indicates if we should wait
     * for user action for the crashing process.
     * in this case, we log a message and turn the debug LED on
     * waiting for a gdb connection (for instance)
     */

    if ((signed)cr.uid <= debug_uid) {
        wait_for_user_action(tid, &cr);
    }

    /* resume stopped process (so it can crash in peace) */
    kill(cr.pid, SIGCONT);

    if (need_cleanup) {
        LOG("debuggerd committing suicide to free the zombie!\n");
        kill(getpid(), SIGKILL);
    }

    if(fd != -1) close(fd);
}

int main()
{
    int s;
    struct sigaction act;

    logsocket = socket_local_client("logd",
            ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_DGRAM);
    if(logsocket < 0) {
        logsocket = -1;
    } else {
        fcntl(logsocket, F_SETFD, FD_CLOEXEC);
    }

    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask,SIGCHLD);
    act.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &act, 0);

    s = socket_local_server("android:debuggerd",
            ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if(s < 0) return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);

    LOG("debuggerd: " __DATE__ " " __TIME__ "\n");

    for(;;) {
        struct sockaddr addr;
        socklen_t alen;
        int fd;

        alen = sizeof(addr);
        fd = accept(s, &addr, &alen);
        if(fd < 0) continue;

        fcntl(fd, F_SETFD, FD_CLOEXEC);

        handle_crashing_process(fd);
    }
    return 0;
}
