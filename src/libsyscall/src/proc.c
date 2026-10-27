#include "sentinel/syscall.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Lots of tedious string formatting and repetative implmentations.
// Done by claude and reviewed by me

static char *read_proc_file(const char *path)
{
    int fd = sc_open_read(path);
    if (fd < 0)
        return NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (NULL == buf) {
        int err = errno;
        sc_close(fd);
        errno = err;
        return NULL;
    }

    while(1) {
        size_t want = cap - len - 1;
        ssize_t got = sc_read_full(fd, buf + len, want);

        if (got < 0) {
            int err = errno;
            free(buf);
            sc_close(fd);
            errno = err;
            return NULL;
        }

        len += (size_t)got;

        // sc_read_full gives fewer bytes than asked only at the end of the file
        if ((size_t)got < want)
            break;

        cap *= 2;
        char *nb = realloc(buf, cap);
        if (NULL == nb) {
            int err = errno;
            free(buf);
            sc_close(fd);
            errno = err;
            return NULL;
        }
        buf = nb;
    }
    buf[len] = '\0';

    if (sc_close(fd) != 0) {
        int err = errno;
        free(buf);
        errno = err;
        return NULL;
    }
    return buf;
}

// the value of a "key: value" line, matched on a whole key at the start of a line
static const char *find_field(const char *text, const char *key)
{
    size_t klen = strlen(key);
    const char *p = text;

    while(NULL != p && '\0' != *p) {
        if (0 == strncmp(p, key, klen) && ':' == p[klen])
            return p + klen + 1;
        p = strchr(p, '\n');
        if (NULL != p)
            p++;
    }
    return NULL;
}

static long status_long(const char *text, const char *key)
{
    const char *v = find_field(text, key);
    if (NULL == v)
        return 0;

    while(' ' == *v || '\t' == *v)
        v++;
    return strtol(v, NULL, 10);
}

static void status_str(const char *text, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';

    const char *v = find_field(text, key);
    if (NULL == v)
        return;

    while(' ' == *v || '\t' == *v)
        v++;

    const char *end = strchr(v, '\n');
    size_t len = NULL != end ? (size_t)(end - v) : strlen(v);
    if (len >= outsz)
        len = outsz - 1;

    memcpy(out, v, len);
    out[len] = '\0';
}

static int count_fd_entries(const char *path, int *err)
{
    DIR *d = opendir(path);
    if (NULL == d) {
        *err = errno;
        return -1;
    }

    int n = 0;
    struct dirent *de;
    while(NULL != (de = readdir(d))) {
        if (0 != strcmp(de->d_name, ".") && 0 != strcmp(de->d_name, ".."))
            n++;
    }
    closedir(d);
    return n;
}

int proc_report(pid_t pid, proc_info_t *out)
{
    if (NULL == out) {
        errno = EINVAL;
        return -1;
    }

    pid_t target = (0 == pid) ? getpid() : pid;
    int self = (target == getpid());

    char path[32];
    if (self)
        snprintf(path, sizeof path, "/proc/self/status");
    else
        snprintf(path, sizeof path, "/proc/%d/status", (int)target);

    char *text = read_proc_file(path);
    if (NULL == text)
        return -1;

    out->pid = target;
    out->ppid = (pid_t)status_long(text, "PPid");
    status_str(text, "Name", out->name, sizeof out->name);
    out->vm_rss_kb = status_long(text, "VmRSS");
    out->vm_size_kb = status_long(text, "VmSize");
    out->threads = (int)status_long(text, "Threads");
    out->vol_ctxt = status_long(text, "voluntary_ctxt_switches");
    out->invol_ctxt = status_long(text, "nonvoluntary_ctxt_switches");
    free(text);

    char fddir[32];
    if (self)
        snprintf(fddir, sizeof fddir, "/proc/self/fd");
    else
        snprintf(fddir, sizeof fddir, "/proc/%d/fd", (int)target);

    int err = 0;
    int n = count_fd_entries(fddir, &err);
    if (n < 0) {
        errno = err;
        return -1;
    }

    // self: exclude proc_report's own opendir fd
    out->open_fds = self ? n - 1 : n;
    return 0;
}

int proc_report_write(int fd, const proc_info_t *info)
{
    if (NULL == info) {
        errno = EINVAL;
        return -1;
    }

    char buf[640];
    int n = snprintf(buf, sizeof buf,
        "process report\n"
        "  %-10s %d\n"
        "  %-10s %d\n"
        "  %-10s %s\n"
        "  %-10s %ld kB\n"
        "  %-10s %ld kB\n"
        "  %-10s %d\n"
        "  %-10s %d\n"
        "  %-10s %ld\n"
        "  %-10s %ld\n",
        "pid", (int)info->pid,
        "ppid", (int)info->ppid,
        "name", info->name,
        "vm_size", info->vm_size_kb,
        "vm_rss", info->vm_rss_kb,
        "threads", info->threads,
        "open_fds", info->open_fds,
        "ctxt_vol", info->vol_ctxt,
        "ctxt_invol", info->invol_ctxt);

    return sc_write_all(fd, buf, (size_t)n);
}
