/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <mtd/mtd-user.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/reboot.h>

#include <cutils/sockets.h>
#include <cutils/iosched_policy.h>
#include <termios.h>
#include <linux/kd.h>
#include <linux/keychord.h>

#include <sys/system_properties.h>

#include "devices.h"
#include "init.h"
#include "property_service.h"
#include "bootchart.h"

static int property_triggers_enabled = 0;

#if BOOTCHART
static int   bootchart_count;
#endif

static char console[32];
static char serialno[32];
static char bootmode[32];
static char baseband[32];
static char carrier[32];
static char bootloader[32];
static char hardware[32];
static unsigned revision = 0;
static char qemu[32];
static struct input_keychord *keychords = 0;
static int keychords_count = 0;
static int keychords_length = 0;

static void notify_service_state(const char *name, const char *state)
{
    char pname[PROP_NAME_MAX];
    int len = strlen(name);
    if ((len + 10) > PROP_NAME_MAX)
        return;
    snprintf(pname, sizeof(pname), "init.svc.%s", name);
    property_set(pname, state);
}

static int have_console;
static char *console_name = "/dev/console";
static time_t process_needs_restart;

static const char *ENV[32];

/* add_environment - add "key=value" to the current environment */
int add_environment(const char *key, const char *val)
{
    int n;
 
    for (n = 0; n < 31; n++) {
        if (!ENV[n]) {
            size_t len = strlen(key) + strlen(val) + 2;
            char *entry = malloc(len);
            snprintf(entry, len, "%s=%s", key, val);
            ENV[n] = entry;
            return 0;
        }
    }

    return 1;
}

static void zap_stdio(void)
{
    int fd;
    fd = open("/dev/null", O_RDWR);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

static void open_console()
{
    int fd;
    if ((fd = open(console_name, O_RDWR)) < 0) {
        fd = open("/dev/null", O_RDWR);
    }
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

/*
 * gettime() - returns the time in seconds of the system's monotonic clock or
 * zero on error.
 */
static time_t gettime(void)
{
    struct timespec ts;
    int ret;

    ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ret < 0) {
        ERROR("clock_gettime(CLOCK_MONOTONIC) failed: %s\n", strerror(errno));
        return 0;
    }

    return ts.tv_sec;
}

static void publish_socket(const char *name, int fd)
{
    char key[64] = ANDROID_SOCKET_ENV_PREFIX;
    char val[64];

    strlcpy(key + sizeof(ANDROID_SOCKET_ENV_PREFIX) - 1,
            name,
            sizeof(key) - sizeof(ANDROID_SOCKET_ENV_PREFIX));
    snprintf(val, sizeof(val), "%d", fd);
    add_environment(key, val);

    /* make sure we don't close-on-exec */
    fcntl(fd, F_SETFD, 0);
}

void service_start(struct service *svc, const char *dynamic_args)
{
    struct stat s;
    pid_t pid;
    int needs_console;
    int n;

        /* starting a service removes it from the disabled
         * state and immediately takes it out of the restarting
         * state if it was in there
         */
    svc->flags &= (~(SVC_DISABLED|SVC_RESTARTING));
    svc->time_started = 0;
    
        /* running processes require no additional work -- if
         * they're in the process of exiting, we've ensured
         * that they will immediately restart on exit, unless
         * they are ONESHOT
         */
    if (svc->flags & SVC_RUNNING) {
        return;
    }

    needs_console = (svc->flags & SVC_CONSOLE) ? 1 : 0;
    if (needs_console && (!have_console)) {
        ERROR("service '%s' requires console\n", svc->name);
        svc->flags |= SVC_DISABLED;
        return;
    }

    if (stat(svc->args[0], &s) != 0) {
        ERROR("cannot find '%s', disabling '%s'\n", svc->args[0], svc->name);
        svc->flags |= SVC_DISABLED;
        return;
    }

    if ((!(svc->flags & SVC_ONESHOT)) && dynamic_args) {
        ERROR("service '%s' must be one-shot to use dynamic args, disabling\n",
               svc->args[0]);
        svc->flags |= SVC_DISABLED;
        return;
    }

    NOTICE("starting '%s'\n", svc->name);

    pid = fork();

    if (pid == 0) {
        struct socketinfo *si;
        struct svcenvinfo *ei;
        char tmp[32];
        int fd, sz;

        get_property_workspace(&fd, &sz);
        sprintf(tmp, "%d,%d", dup(fd), sz);
        add_environment("ANDROID_PROPERTY_WORKSPACE", tmp);

        for (ei = svc->envvars; ei; ei = ei->next)
            add_environment(ei->name, ei->value);

        for (si = svc->sockets; si; si = si->next) {
            int s = create_socket(si->name,
                                  !strcmp(si->type, "dgram") ? 
                                  SOCK_DGRAM : SOCK_STREAM,
                                  si->perm, si->uid, si->gid);
            if (s >= 0) {
                publish_socket(si->name, s);
            }
        }

        if (svc->ioprio_class != IoSchedClass_NONE) {
            if (android_set_ioprio(getpid(), svc->ioprio_class, svc->ioprio_pri)) {
                ERROR("Failed to set pid %d ioprio = %d,%d: %s\n",
                      getpid(), svc->ioprio_class, svc->ioprio_pri, strerror(errno));
            }
        }

        if (needs_console) {
            setsid();
            open_console();
        } else {
            zap_stdio();
        }

#if 0
        for (n = 0; svc->args[n]; n++) {
            INFO("args[%d] = '%s'\n", n, svc->args[n]);
        }
        for (n = 0; ENV[n]; n++) {
            INFO("env[%d] = '%s'\n", n, ENV[n]);
        }
#endif

        setpgid(0, getpid());

    /* as requested, set our gid, supplemental gids, and uid */
        if (svc->gid) {
            setgid(svc->gid);
        }
        if (svc->nr_supp_gids) {
            setgroups(svc->nr_supp_gids, svc->supp_gids);
        }
        if (svc->uid) {
            setuid(svc->uid);
        }

        if (!dynamic_args) {
            if (execve(svc->args[0], (char**) svc->args, (char**) ENV) < 0) {
                ERROR("cannot execve('%s'): %s\n", svc->args[0], strerror(errno));
            }
        } else {
            char *arg_ptrs[SVC_MAXARGS+1];
            int arg_idx = svc->nargs;
            char *tmp = strdup(dynamic_args);
            char *next = tmp;
            char *bword;

            /* Copy the static arguments */
            memcpy(arg_ptrs, svc->args, (svc->nargs * sizeof(char *)));

            while((bword = strsep(&next, " "))) {
                arg_ptrs[arg_idx++] = bword;
                if (arg_idx == SVC_MAXARGS)
                    break;
            }
            arg_ptrs[arg_idx] = '\0';
            execve(svc->args[0], (char**) arg_ptrs, (char**) ENV);
        }
        _exit(127);
    }

    if (pid < 0) {
        ERROR("failed to start '%s'\n", svc->name);
        svc->pid = 0;
        return;
    }

    svc->time_started = gettime();
    svc->pid = pid;
    svc->flags |= SVC_RUNNING;

    notify_service_state(svc->name, "running");
}

void service_stop(struct service *svc)
{
        /* we are no longer running, nor should we
         * attempt to restart
         */
    svc->flags &= (~(SVC_RUNNING|SVC_RESTARTING));

        /* if the service has not yet started, prevent
         * it from auto-starting with its class
         */
    svc->flags |= SVC_DISABLED;

    if (svc->pid) {
        NOTICE("service '%s' is being killed\n", svc->name);
        kill(-svc->pid, SIGTERM);
        notify_service_state(svc->name, "stopping");
    } else {
        notify_service_state(svc->name, "stopped");
    }
}

void property_changed(const char *name, const char *value)
{
    if (property_triggers_enabled) {
        queue_property_triggers(name, value);
        drain_action_queue();
    }
}

#define CRITICAL_CRASH_THRESHOLD    4       /* if we crash >4 times ... */
#define CRITICAL_CRASH_WINDOW       (4*60)  /* ... in 4 minutes, goto recovery*/

static int wait_for_one_process(int block)
{
    pid_t pid;
    int status;
    struct service *svc;
    struct socketinfo *si;
    time_t now;
    struct listnode *node;
    struct command *cmd;

    while ( (pid = waitpid(-1, &status, block ? 0 : WNOHANG)) == -1 && errno == EINTR );
    if (pid <= 0) return -1;
    INFO("waitpid returned pid %d, status = %08x\n", pid, status);

    svc = service_find_by_pid(pid);
    if (!svc) {
        ERROR("untracked pid %d exited\n", pid);
        return 0;
    }

    NOTICE("process '%s', pid %d exited\n", svc->name, pid);

    if (!(svc->flags & SVC_ONESHOT)) {
        kill(-pid, SIGKILL);
        NOTICE("process '%s' killing any children in process group\n", svc->name);
    }

    /* remove any sockets we may have created */
    for (si = svc->sockets; si; si = si->next) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), ANDROID_SOCKET_DIR"/%s", si->name);
        unlink(tmp);
    }

    svc->pid = 0;
    svc->flags &= (~SVC_RUNNING);

        /* oneshot processes go into the disabled state on exit */
    if (svc->flags & SVC_ONESHOT) {
        svc->flags |= SVC_DISABLED;
    }

        /* disabled processes do not get restarted automatically */
    if (svc->flags & SVC_DISABLED) {
        notify_service_state(svc->name, "stopped");
        return 0;
    }

    now = gettime();
    if (svc->flags & SVC_CRITICAL) {
        if (svc->time_crashed + CRITICAL_CRASH_WINDOW >= now) {
            if (++svc->nr_crashed > CRITICAL_CRASH_THRESHOLD) {
                ERROR("critical process '%s' exited %d times in %d minutes; "
                      "rebooting into recovery mode\n", svc->name,
                      CRITICAL_CRASH_THRESHOLD, CRITICAL_CRASH_WINDOW / 60);
                sync();
                __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                         LINUX_REBOOT_CMD_RESTART2, "recovery");
                return 0;
            }
        } else {
            svc->time_crashed = now;
            svc->nr_crashed = 1;
        }
    }

    svc->flags |= SVC_RESTARTING;

    /* Execute all onrestart commands for this service. */
    list_for_each(node, &svc->onrestart.commands) {
        cmd = node_to_item(node, struct command, clist);
        cmd->func(cmd->nargs, cmd->args);
    }
    notify_service_state(svc->name, "restarting");
    return 0;
}

static void restart_service_if_needed(struct service *svc)
{
    time_t next_start_time = svc->time_started + 5;

    if (next_start_time <= gettime()) {
        svc->flags &= (~SVC_RESTARTING);
        service_start(svc, NULL);
        return;
    }

    if ((next_start_time < process_needs_restart) ||
        (process_needs_restart == 0)) {
        process_needs_restart = next_start_time;
    }
}

static void restart_processes()
{
    process_needs_restart = 0;
    service_for_each_flags(SVC_RESTARTING,
                           restart_service_if_needed);
}

static int signal_fd = -1;

static void sigchld_handler(int s)
{
    write(signal_fd, &s, 1);
}

static void msg_start(const char *name)
{
    struct service *svc;
    char *tmp = NULL;
    char *args = NULL;

    if (!strchr(name, ':'))
        svc = service_find_by_name(name);
    else {
        tmp = strdup(name);
        args = strchr(tmp, ':');
        *args = '\0';
        args++;

        svc = service_find_by_name(tmp);
    }
    
    if (svc) {
        service_start(svc, args);
    } else {
        ERROR("no such service '%s'\n", name);
    }
    if (tmp)
        free(tmp);
}

static void msg_stop(const char *name)
{
    struct service *svc = service_find_by_name(name);

    if (svc) {
        service_stop(svc);
    } else {
        ERROR("no such service '%s'\n", name);
    }
}

void handle_control_message(const char *msg, const char *arg)
{
    if (!strcmp(msg,"start")) {
        msg_start(arg);
    } else if (!strcmp(msg,"stop")) {
        msg_stop(arg);
    } else {
        ERROR("unknown control msg '%s'\n", msg);
    }
}

#define MAX_MTD_PARTITIONS 16

static struct {
    char name[16];
    int number;
} mtd_part_map[MAX_MTD_PARTITIONS];

static int mtd_part_count = -1;

static void find_mtd_partitions(void)
{
    int fd;
    char buf[1024];
    char *pmtdbufp;
    ssize_t pmtdsize;
    int r;

    fd = open("/proc/mtd", O_RDONLY);
    if (fd < 0)
        return;

    buf[sizeof(buf) - 1] = '\0';
    pmtdsize = read(fd, buf, sizeof(buf) - 1);
    pmtdbufp = buf;
    while (pmtdsize > 0) {
        int mtdnum, mtdsize, mtderasesize;
        char mtdname[16];
        mtdname[0] = '\0';
        mtdnum = -1;
        r = sscanf(pmtdbufp, "mtd%d: %x %x %15s",
                   &mtdnum, &mtdsize, &mtderasesize, mtdname);
        if ((r == 4) && (mtdname[0] == '"')) {
            char *x = strchr(mtdname + 1, '"');
            if (x) {
                *x = 0;
            }
            INFO("mtd partition %d, %s\n", mtdnum, mtdname + 1);
            if (mtd_part_count < MAX_MTD_PARTITIONS) {
                strcpy(mtd_part_map[mtd_part_count].name, mtdname + 1);
                mtd_part_map[mtd_part_count].number = mtdnum;
                mtd_part_count++;
            } else {
                ERROR("too many mtd partitions\n");
            }
        }
        while (pmtdsize > 0 && *pmtdbufp != '\n') {
            pmtdbufp++;
            pmtdsize--;
        }
        if (pmtdsize > 0) {
            pmtdbufp++;
            pmtdsize--;
        }
    }
    close(fd);
}

int mtd_name_to_number(const char *name) 
{
    int n;
    if (mtd_part_count < 0) {
        mtd_part_count = 0;
        find_mtd_partitions();
    }
    for (n = 0; n < mtd_part_count; n++) {
        if (!strcmp(name, mtd_part_map[n].name)) {
            return mtd_part_map[n].number;
        }
    }
    return -1;
}

static void import_kernel_nv(char *name, int in_qemu)
{
    char *value = strchr(name, '=');

    if (value == 0) return;
    *value++ = 0;
    if (*name == 0) return;

    if (!in_qemu)
    {
        /* on a real device, white-list the kernel options */
        if (!strcmp(name,"qemu")) {
            strlcpy(qemu, value, sizeof(qemu));
        } else if (!strcmp(name,"androidboot.console")) {
            strlcpy(console, value, sizeof(console));
        } else if (!strcmp(name,"androidboot.mode")) {
            strlcpy(bootmode, value, sizeof(bootmode));
        } else if (!strcmp(name,"androidboot.serialno")) {
            strlcpy(serialno, value, sizeof(serialno));
        } else if (!strcmp(name,"androidboot.baseband")) {
            strlcpy(baseband, value, sizeof(baseband));
        } else if (!strcmp(name,"androidboot.carrier")) {
            strlcpy(carrier, value, sizeof(carrier));
        } else if (!strcmp(name,"androidboot.bootloader")) {
            strlcpy(bootloader, value, sizeof(bootloader));
        } else if (!strcmp(name,"androidboot.hardware")) {
            strlcpy(hardware, value, sizeof(hardware));
        } else {
            qemu_cmdline(name, value);
        }
    } else {
        /* in the emulator, export any kernel option with the
         * ro.kernel. prefix */
        char  buff[32];
        int   len = snprintf( buff, sizeof(buff), "ro.kernel.%s", name );
        if (len < (int)sizeof(buff)) {
            property_set( buff, value );
        }
    }
}

static void import_kernel_cmdline(int in_qemu)
{
    char cmdline[1024];
    char *ptr;
    int fd;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, cmdline, 1023);
        if (n < 0) n = 0;

        /* get rid of trailing newline, it happens */
        if (n > 0 && cmdline[n-1] == '\n') n--;

        cmdline[n] = 0;
        close(fd);
    } else {
        cmdline[0] = 0;
    }

    ptr = cmdline;
    while (ptr && *ptr) {
        char *x = strchr(ptr, ' ');
        if (x != 0) *x++ = 0;
        import_kernel_nv(ptr, in_qemu);
        ptr = x;
    }

        /* don't expose the raw commandline to nonpriv processes */
    chmod("/proc/cmdline", 0440);
}

static void get_hardware_name(void)
{
    char data[1024];
    int fd, n;
    char *x, *hw, *rev;

    /* Hardware string was provided on kernel command line */
    if (hardware[0])
        return;

    fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) return;

    n = read(fd, data, 1023);
    close(fd);
    if (n < 0) return;

    data[n] = 0;
    hw = strstr(data, "\nHardware");
    rev = strstr(data, "\nRevision");

    if (hw) {
        x = strstr(hw, ": ");
        if (x) {
            x += 2;
            n = 0;
            while (*x && !isspace(*x)) {
                hardware[n++] = tolower(*x);
                x++;
                if (n == 31) break;
            }
            hardware[n] = 0;
        }
    }

    if (rev) {
        x = strstr(rev, ": ");
        if (x) {
            revision = strtoul(x + 2, 0, 16);
        }
    }
}

void drain_action_queue(void)
{
    struct listnode *node;
    struct command *cmd;
    struct action *act;
    int ret;

    while ((act = action_remove_queue_head())) {
        INFO("processing action %p (%s)\n", act, act->name);
        list_for_each(node, &act->commands) {
            cmd = node_to_item(node, struct command, clist);
            ret = cmd->func(cmd->nargs, cmd->args);
            INFO("command '%s' r=%d\n", cmd->args[0], ret);
        }
    }
}

void open_devnull_stdio(void)
{
    int fd;
    static const char *name = "/dev/__null__";
    if (mknod(name, S_IFCHR | 0600, (1 << 8) | 3) == 0) {
        fd = open(name, O_RDWR);
        unlink(name);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) {
                close(fd);
            }
            return;
        }
    }

    exit(1);
}

void add_service_keycodes(struct service *svc)
{
    struct input_keychord *keychord;
    int i, size;

    if (svc->keycodes) {
        /* add a new keychord to the list */
        size = sizeof(*keychord) + svc->nkeycodes * sizeof(keychord->keycodes[0]);
        keychords = realloc(keychords, keychords_length + size);
        if (!keychords) {
            ERROR("could not allocate keychords\n");
            keychords_length = 0;
            keychords_count = 0;
            return;
        }

        keychord = (struct input_keychord *)((char *)keychords + keychords_length);
        keychord->version = KEYCHORD_VERSION;
        keychord->id = keychords_count + 1;
        keychord->count = svc->nkeycodes;
        svc->keychord_id = keychord->id;

        for (i = 0; i < svc->nkeycodes; i++) {
            keychord->keycodes[i] = svc->keycodes[i];
        }
        keychords_count++;
        keychords_length += size;
    }
}

int open_keychord()
{
    int fd, ret;

    service_for_each(add_service_keycodes);
    
    /* nothing to do if no services require keychords */
    if (!keychords)
        return -1;

    fd = open("/dev/keychord", O_RDWR);
    if (fd < 0) {
        ERROR("could not open /dev/keychord\n");
        return fd;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    ret = write(fd, keychords, keychords_length);
    if (ret != keychords_length) {
        ERROR("could not configure /dev/keychord %d (%d)\n", ret, errno);
        close(fd);
        fd = -1;
    }

    free(keychords);
    keychords = 0;

    return fd;
}

void handle_keychord(int fd)
{
    struct service *svc;
    char* debuggable;
    char* adb_enabled;
    int ret;
    __u16 id;

    // only handle keychords if ro.debuggable is set or adb is enabled.
    // the logic here is that bugreports should be enabled in userdebug or eng builds
    // and on user builds for users that are developers.
    debuggable = property_get("ro.debuggable");
    adb_enabled = property_get("init.svc.adbd");
    if ((debuggable && !strcmp(debuggable, "1")) ||
        (adb_enabled && !strcmp(adb_enabled, "running"))) {
        ret = read(fd, &id, sizeof(id));
        if (ret != sizeof(id)) {
            ERROR("could not read keychord id\n");
            return;
        }

        svc = service_find_by_keychord(id);
        if (svc) {
            INFO("starting service %s from keychord\n", svc->name);
            service_start(svc, NULL);
        } else {
            ERROR("service for keychord %d not found\n", id);
        }
    }
}

int main(int argc, char **argv)
{
    int device_fd = -1;
    int property_set_fd = -1;
    int signal_recv_fd = -1;
    int keychord_fd = -1;
    int fd_count;
    int s[2];
    int fd;
    struct sigaction act;
    char tmp[PROP_VALUE_MAX];
    struct pollfd ufds[4];
    char *tmpdev;
    char* debuggable;

    act.sa_handler = sigchld_handler;
    act.sa_flags = SA_NOCLDSTOP;
    act.sa_mask = 0;
    act.sa_restorer = NULL;
    sigaction(SIGCHLD, &act, 0);

    /* clear the umask */
    umask(0);

        /* Get the basic filesystem setup we need put
         * together in the initramdisk on / and then we'll
         * let the rc file figure out the rest.
         */
    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);

    mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    mkdir("/dev/pts", 0755);
    mkdir("/dev/socket", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

        /* We must have some place other than / to create the
         * device nodes for kmsg and null, otherwise we won't
         * be able to remount / read-only later on.
         * Now that tmpfs is mounted on /dev, we can actually
         * talk to the outside world.
         */
    open_devnull_stdio();
    log_init();
    
    INFO("reading config file\n");
    parse_config_file("/init.rc");

    /* pull the kernel commandline and ramdisk properties file in */
    qemu_init();
    import_kernel_cmdline(0);

    get_hardware_name();
    snprintf(tmp, sizeof(tmp), "/init.%s.rc", hardware);
    parse_config_file(tmp);

    action_for_each_trigger("early-init", action_add_queue_tail);
    drain_action_queue();

    INFO("device init\n");
    device_fd = device_init();

    property_init();
    
    // only listen for keychords if ro.debuggable is true
    keychord_fd = open_keychord();

    if (console[0]) {
        snprintf(tmp, sizeof(tmp), "/dev/%s", console);
        console_name = strdup(tmp);
    }

    fd = open(console_name, O_RDWR);
    if (fd >= 0)
        have_console = 1;
    close(fd);

    if( load_565rle_image(INIT_IMAGE_FILE) ) {
    fd = open("/dev/tty0", O_WRONLY);
    if (fd >= 0) {
        const char *msg;
            msg = "\n"
        "\n"
        "\n"
        "\n"
        "\n"
        "\n"
        "\n"  // console is 40 cols x 30 lines
        "\n"
        "\n"
        "\n"
        "\n"
        "\n"
        "\n"
        "\n"
        "             A N D R O I D ";
        write(fd, msg, strlen(msg));
        close(fd);
    }
    }

    if (qemu[0])
        import_kernel_cmdline(1); 

    if (!strcmp(bootmode,"factory"))
        property_set("ro.factorytest", "1");
    else if (!strcmp(bootmode,"factory2"))
        property_set("ro.factorytest", "2");
    else
        property_set("ro.factorytest", "0");

    property_set("ro.serialno", serialno[0] ? serialno : "");
    property_set("ro.bootmode", bootmode[0] ? bootmode : "unknown");
    property_set("ro.baseband", baseband[0] ? baseband : "unknown");
    property_set("ro.carrier", carrier[0] ? carrier : "unknown");
    property_set("ro.bootloader", bootloader[0] ? bootloader : "unknown");

    property_set("ro.hardware", hardware);
    snprintf(tmp, PROP_VALUE_MAX, "%d", revision);
    property_set("ro.revision", tmp);

        /* execute all the boot actions to get us started */
    action_for_each_trigger("init", action_add_queue_tail);
    drain_action_queue();

        /* read any property files on system or data and
         * fire up the property service.  This must happen
         * after the ro.foo properties are set above so
         * that /data/local.prop cannot interfere with them.
         */
    property_set_fd = start_property_service();

    /* create a signalling mechanism for the sigchld handler */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
        signal_fd = s[0];
        signal_recv_fd = s[1];
        fcntl(s[0], F_SETFD, FD_CLOEXEC);
        fcntl(s[0], F_SETFL, O_NONBLOCK);
        fcntl(s[1], F_SETFD, FD_CLOEXEC);
        fcntl(s[1], F_SETFL, O_NONBLOCK);
    }

    /* make sure we actually have all the pieces we need */
    if ((device_fd < 0) ||
        (property_set_fd < 0) ||
        (signal_recv_fd < 0)) {
        ERROR("init startup failure\n");
        return 1;
    }

    /* execute all the boot actions to get us started */
    action_for_each_trigger("early-boot", action_add_queue_tail);
    action_for_each_trigger("boot", action_add_queue_tail);
    drain_action_queue();

        /* run all property triggers based on current state of the properties */
    queue_all_property_triggers();
    drain_action_queue();

        /* enable property triggers */   
    property_triggers_enabled = 1;     

    ufds[0].fd = device_fd;
    ufds[0].events = POLLIN;
    ufds[1].fd = property_set_fd;
    ufds[1].events = POLLIN;
    ufds[2].fd = signal_recv_fd;
    ufds[2].events = POLLIN;
    fd_count = 3;

    if (keychord_fd > 0) {
        ufds[3].fd = keychord_fd;
        ufds[3].events = POLLIN;
        fd_count++;
    } else {
        ufds[3].events = 0;
        ufds[3].revents = 0;
    }

#if BOOTCHART
    bootchart_count = bootchart_init();
    if (bootchart_count < 0) {
        ERROR("bootcharting init failure\n");
    } else if (bootchart_count > 0) {
        NOTICE("bootcharting started (period=%d ms)\n", bootchart_count*BOOTCHART_POLLING_MS);
    } else {
        NOTICE("bootcharting ignored\n");
    }
#endif

    for(;;) {
        int nr, i, timeout = -1;

        for (i = 0; i < fd_count; i++)
            ufds[i].revents = 0;

        drain_action_queue();
        restart_processes();

        if (process_needs_restart) {
            timeout = (process_needs_restart - gettime()) * 1000;
            if (timeout < 0)
                timeout = 0;
        }

#if BOOTCHART
        if (bootchart_count > 0) {
            if (timeout < 0 || timeout > BOOTCHART_POLLING_MS)
                timeout = BOOTCHART_POLLING_MS;
            if (bootchart_step() < 0 || --bootchart_count == 0) {
                bootchart_finish();
                bootchart_count = 0;
            }
        }
#endif
        nr = poll(ufds, fd_count, timeout);
        if (nr <= 0)
            continue;

        if (ufds[2].revents == POLLIN) {
            /* we got a SIGCHLD - reap and restart as needed */
            read(signal_recv_fd, tmp, sizeof(tmp));
            while (!wait_for_one_process(0))
                ;
            continue;
        }

        if (ufds[0].revents == POLLIN)
            handle_device_fd(device_fd);

        if (ufds[1].revents == POLLIN)
            handle_property_set_fd(property_set_fd);
        if (ufds[3].revents == POLLIN)
            handle_keychord(keychord_fd);
    }

    return 0;
}
