/**
 * (C) Copyright 2022-2024 Intel Corporation.
 * (C) Copyright 2025-2026 Hewlett Packard Enterprise Development LP
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Adapted from DAOS pil4dfs/int_dfs.c and pil4dfs_int.h, commit
 * dcb0e18cf66264857a78b31e1e96c4d0648b08af. See third_party/daos/NOTICE.md.
 * Copied dispatch routines retain their original names. DFS operations are
 * replaced with libfuse3 callbacks. Single mount, no dup/compatible mode.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Constants retained from pil4dfs_int.h. */
#define FD_FILE_BASE 0x20000000
#define MAX_OPENED_FILE 2048
struct dfs_mt { char *fs_root; int len_fs_root; };
static struct dfs_mt dfs_list[1];
static int num_dfs, enabled;
#define d_hook_enabled (enabled && !fb_internal)
struct file_obj {
    char *path;
    struct fuse_file_info fi;
    int open_flag;
    off_t offset;
};
static struct file_obj *d_file_list[MAX_OPENED_FILE];
static pthread_mutex_t lock_fd = PTHREAD_MUTEX_INITIALIZER;
static unsigned long num_open, num_read, num_write, num_forward;
static char **nested_mounts;
static size_t nested_count;
static int config_error;
static int (*libc_open)(const char *, int, ...);
static int (*libc_close)(int);
static ssize_t (*libc_read)(int, void *, size_t);
static ssize_t (*libc_write)(int, const void *, size_t);
static ssize_t (*next_pread)(int, void *, size_t, off_t);
static ssize_t (*next_pwrite)(int, const void *, size_t, off_t);
static off_t (*libc_lseek)(int, off_t, int);
static int (*next_fsync)(int), (*next_fdatasync)(int);

static int failure(int e) { errno = e; return -1; }
static int posix_result(int r) { return r < 0 ? failure(-r) : r; }
static int original_open(const char *p, int flags, mode_t mode)
{
    if (libc_open) return libc_open(p, flags, mode);
    return (int)syscall(SYS_openat, AT_FDCWD, p, flags, mode);
}
static int has_mode(int flags)
{
    return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
}
/* Copied verbatim from DAOS query_dfs_mount. */
static int
query_dfs_mount(const char *path)
{
    int i, idx = -1, max_len = -1;

    for (i = 0; i < num_dfs; i++) {
        /* To find in existing list with max length */
        if (strncmp(path, dfs_list[i].fs_root, dfs_list[i].len_fs_root) == 0 &&
            (path[dfs_list[i].len_fs_root] == '/' ||
             path[dfs_list[i].len_fs_root] == 0)) {
            if (dfs_list[i].len_fs_root > max_len) {
                idx = i;
                max_len = dfs_list[i].len_fs_root;
            }
        }
    }
    /* -1 means the mount point is not found in dfs_list. */
    return idx;
}
/* Restricted query_path: unsupported syntax goes wholly through libc. */
static int query_path(const char *p, const char **inside)
{
    if (!p || p[0] != '/' || strlen(p) >= PATH_MAX) return 0;
    for (const char *s = p + 1; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && !memcmp(s, "..", 2))) return 0;
        if (!e) break;
        s = e + 1;
        if (!*s) return 0;
    }
    int idx = query_dfs_mount(p);
    if (idx < 0) return 0;
    for (size_t i = 0; i < nested_count; i++) {
        size_t n = strlen(nested_mounts[i]);
        if (!strncmp(p, nested_mounts[i], n) && (!p[n] || p[n] == '/')) return 0;
    }
    *inside = p + dfs_list[idx].len_fs_root;
    if (!**inside) *inside = "/";
    return 1;
}
/* DAOS's ordinary mode needs no kernel-fd redirection. */
static int d_get_fd_redirected(int fd) { return fd; }
static struct file_obj *lookup(int fd)
{
    if (fd < FD_FILE_BASE || fd >= FD_FILE_BASE + MAX_OPENED_FILE) return NULL;
    return d_file_list[fd - FD_FILE_BASE];
}

/* Validate components through the target filesystem, not the mounted view.
 * Return 1 for whole-file kernel fallback, 0 for regular/missing leaf. */
static int check_path(const char *path, struct stat *st, int *exists)
{
    if (!fb_ops.getattr) return 1;
    char copy[PATH_MAX];
    strcpy(copy, path);
    for (char *p = copy + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        int r = fb_ops.getattr(copy, st, NULL);
        *p = saved;
        if (r == -ENOENT && !saved) { *exists = 0; return 0; }
        if (r < 0) return r;
        if (S_ISLNK(st->st_mode)) return 1;
        if (saved && !S_ISDIR(st->st_mode)) return -ENOTDIR;
        if (!saved) { *exists = 1; return S_ISREG(st->st_mode) ? 0 : 1; }
    }
}

/* Port of open_common's query -> target open/table -> org_func structure.
 * DFS lookup/create and object fields are replaced by FUSE callbacks. */
static int open_common(const char *pathname, int oflags, mode_t mode)
{
    const char *path;
    int rc, exists = 0, idx_fd = -1;
    struct stat st;
    struct file_obj *obj;
    if (!d_hook_enabled || !query_path(pathname, &path)) goto org_func;
    if (config_error) return failure(config_error);
    int supported = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_LARGEFILE;
    if ((oflags & ~supported) || (oflags & O_ACCMODE) == O_ACCMODE ||
        ((oflags & O_TRUNC) && (oflags & O_ACCMODE) == O_RDONLY)) goto org_func;
    rc = fb_start();
    if (rc) return posix_result(rc);
    if (((oflags & O_ACCMODE) != O_WRONLY && !fb_ops.read) ||
        ((oflags & O_ACCMODE) != O_RDONLY && !fb_ops.write) ||
        ((oflags & O_TRUNC) && !fb_ops.truncate) ||
        ((oflags & O_CREAT) && !fb_ops.create)) goto org_func;
    pthread_mutex_lock(&lock_fd);
    fb_enter();
    rc = check_path(path, &st, &exists);
    if (rc == 1) { fb_leave(); pthread_mutex_unlock(&lock_fd); goto org_func; }
    if (rc < 0) goto error;
    if (!exists && !(oflags & O_CREAT)) { rc = -ENOENT; goto error; }
    if (exists && (oflags & O_CREAT) && (oflags & O_EXCL)) { rc = -EEXIST; goto error; }
    for (int i = 0; i < MAX_OPENED_FILE; i++) if (!d_file_list[i]) { idx_fd = i; break; }
    if (idx_fd < 0) { rc = -EMFILE; goto error; }
    obj = calloc(1, sizeof(*obj));
    if (!obj) { rc = -ENOMEM; goto error; }
    obj->path = strdup(path);
    if (!obj->path) { free(obj); rc = -ENOMEM; goto error; }
    obj->open_flag = oflags;
    obj->fi.flags = oflags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (!exists)
        rc = fb_ops.create(path, mode & ~fuse_get_context()->umask, &obj->fi);
    else
        rc = fb_ops.open ? fb_ops.open(path, &obj->fi) : 0;
    if (rc < 0) { free(obj->path); free(obj); goto error; }
    if (exists && (oflags & O_TRUNC)) {
        rc = fb_ops.truncate(path, 0, &obj->fi);
        if (rc < 0) { if (fb_ops.release) fb_ops.release(path, &obj->fi); free(obj->path); free(obj); goto error; }
    }
    d_file_list[idx_fd] = obj;
    num_open++;
    fb_leave(); pthread_mutex_unlock(&lock_fd);
    return idx_fd + FD_FILE_BASE;
error:
    fb_leave(); pthread_mutex_unlock(&lock_fd);
    return posix_result(rc);
org_func:
    if (!fb_internal) __sync_fetch_and_add(&num_forward, 1);
    return original_open(pathname, oflags, mode);
}

int open(const char *p, int flags, ...)
{
    mode_t mode = 0;
    if (has_mode(flags)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, unsigned int); va_end(ap); }
    return open_common(p, flags, mode);
}
int open64(const char *, int, ...) __attribute__((alias("open")));
int creat(const char *p, mode_t m) { return open_common(p, O_WRONLY | O_CREAT | O_TRUNC, m); }
int creat64(const char *, mode_t) __attribute__((alias("creat")));
extern void __chk_fail(void) __attribute__((noreturn));
int __open_2(const char *p, int flags) { if (has_mode(flags)) __chk_fail(); return open_common(p, flags, 0); }
int __open64_2(const char *, int) __attribute__((alias("__open_2")));

/* Callback adapters replacing pread_over_dfs/pwrite_over_dfs. Caller owns lock. */
static ssize_t io_over_fuse(int index, void *buf, size_t size, off_t offset, int writing)
{
    struct file_obj *f = d_file_list[index];
    if ((writing && (f->open_flag & O_ACCMODE) == O_RDONLY) ||
        (!writing && (f->open_flag & O_ACCMODE) == O_WRONLY)) return failure(EBADF);
    if (offset < 0) return failure(EINVAL);
    if (!buf && size) return failure(EFAULT);
    if (size > INT_MAX) size = INT_MAX;
    if ((uint64_t)offset + size > INT64_MAX) return failure(EOVERFLOW);
    if (!size) return 0;
    fb_enter();
    int r;
    if (writing) { num_write++; r = fb_ops.write(f->path, buf, size, offset, &f->fi); }
    else { num_read++; r = fb_ops.read(f->path, buf, size, offset, &f->fi); }
    fb_leave();
    if (r > (int)size) return failure(EIO);
    return posix_result(r);
}
static ssize_t pread_over_dfs(int i, void *b, size_t n, off_t o) { return io_over_fuse(i, b, n, o, 0); }
static ssize_t pwrite_over_dfs(int i, const void *b, size_t n, off_t o) { return io_over_fuse(i, (void *)b, n, o, 1); }

/* DAOS read_comm/write_comm inserted below by the source extraction step. */

static ssize_t
read_comm(ssize_t (*next_read)(int fd, void *buf, size_t size), int fd, void *buf, size_t size)
{
	ssize_t rc;
	int	fd_directed;

	if (!d_hook_enabled)
		return next_read(fd, buf, size);

	fd_directed = d_get_fd_redirected(fd);
	if (fd_directed >= FD_FILE_BASE) {
		rc = pread_over_dfs(fd_directed - FD_FILE_BASE, buf, size,
				    d_file_list[fd_directed - FD_FILE_BASE]->offset);
		if (rc >= 0)
			d_file_list[fd_directed - FD_FILE_BASE]->offset += rc;
		return rc;
	} else {
		return next_read(fd_directed, buf, size);
	}
}


static ssize_t
write_comm(ssize_t (*next_write)(int fd, const void *buf, size_t size), int fd, const void *buf,
	   size_t size)
{
	ssize_t	rc;
	int	fd_directed;

	if (!d_hook_enabled)
		return next_write(fd, buf, size);

	fd_directed = d_get_fd_redirected(fd);
	if (fd_directed >= FD_FILE_BASE) {
		rc = pwrite_over_dfs(fd_directed - FD_FILE_BASE, buf, size,
				     d_file_list[fd_directed - FD_FILE_BASE]->offset);
		if (rc >= 0)
			d_file_list[fd_directed - FD_FILE_BASE]->offset += rc;
		return rc;
	} else {
		return next_write(fd_directed, buf, size);
	}
}


/* Wrappers add bounds/lifetime checks before the copied DAOS dispatch code. */
ssize_t read(int fd, void *buf, size_t n)
{
    if (!libc_read) return syscall(SYS_read, fd, buf, n);
    if (!d_hook_enabled || fd < FD_FILE_BASE) return libc_read(fd, buf, n);
    pthread_mutex_lock(&lock_fd);
    ssize_t r = lookup(fd) ? read_comm(libc_read, fd, buf, n) : failure(EBADF);
    pthread_mutex_unlock(&lock_fd); return r;
}
ssize_t write(int fd, const void *buf, size_t n)
{
    if (!libc_write) return syscall(SYS_write, fd, buf, n);
    if (!d_hook_enabled || fd < FD_FILE_BASE) return libc_write(fd, buf, n);
    pthread_mutex_lock(&lock_fd);
    ssize_t r = lookup(fd) ? write_comm(libc_write, fd, buf, n) : failure(EBADF);
    pthread_mutex_unlock(&lock_fd); return r;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    if (!next_pread) return syscall(SYS_pread64, fd, buf, n, off);
    if (!d_hook_enabled) return next_pread(fd, buf, n, off);
    int fd_directed = d_get_fd_redirected(fd);
    if (fd_directed < FD_FILE_BASE) return next_pread(fd, buf, n, off);
    pthread_mutex_lock(&lock_fd);
    ssize_t r = lookup(fd_directed) ? pread_over_dfs(fd_directed - FD_FILE_BASE, buf, n, off) : failure(EBADF);
    pthread_mutex_unlock(&lock_fd); return r;
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (!next_pwrite) return syscall(SYS_pwrite64, fd, buf, n, off);
    if (!d_hook_enabled) return next_pwrite(fd, buf, n, off);
    int fd_directed = d_get_fd_redirected(fd);
    if (fd_directed < FD_FILE_BASE) return next_pwrite(fd, buf, n, off);
    pthread_mutex_lock(&lock_fd);
    ssize_t r = lookup(fd_directed) ? pwrite_over_dfs(fd_directed - FD_FILE_BASE, buf, n, off) : failure(EBADF);
    pthread_mutex_unlock(&lock_fd); return r;
}
ssize_t pread64(int, void *, size_t, off_t) __attribute__((alias("pread")));
ssize_t pwrite64(int, const void *, size_t, off_t) __attribute__((alias("pwrite")));
ssize_t __pread64(int, void *, size_t, off_t) __attribute__((alias("pread")));
ssize_t __pwrite64(int, const void *, size_t, off_t) __attribute__((alias("pwrite")));
ssize_t __read_chk(int fd, void *b, size_t n, size_t cap) { if (n > cap) __chk_fail(); return read(fd,b,n); }
ssize_t __pread64_chk(int fd, void *b, size_t n, off_t o, size_t cap) { if (n > cap) __chk_fail(); return pread(fd,b,n,o); }

/* Adapted from lseek_comm: preserve switch/offset behavior; add overflow
 * checks, use getattr instead of new_fxstat, omit directory/dup modes. */
off_t lseek(int fd, off_t off, int whence)
{
    if (!libc_lseek) return syscall(SYS_lseek, fd, off, whence);
    if (!d_hook_enabled || fd < FD_FILE_BASE) return libc_lseek(fd, off, whence);
    pthread_mutex_lock(&lock_fd);
    struct file_obj *f = lookup(fd);
    off_t base = 0, result = -1;
    if (!f) { errno = EBADF; goto out; }
    if (f->fi.nonseekable) { errno = ESPIPE; goto out; }
    switch (whence) {
    case SEEK_SET: break;
    case SEEK_CUR: base = f->offset; break;
    case SEEK_END: {
        struct stat st;
        fb_enter();
        int r = fb_ops.getattr(f->path, &st, &f->fi);
        fb_leave();
        if (r < 0) { errno = -r; goto out; }
        base = st.st_size; break;
    }
    default: errno = EINVAL; goto out;
    }
    if (base < 0 || (off > 0 && base > INT64_MAX - off)) { errno = EOVERFLOW; goto out; }
    if (off < -base) { errno = EINVAL; goto out; }
    result = f->offset = base + off;
out:
    pthread_mutex_unlock(&lock_fd); return result;
}
off_t lseek64(int, off_t, int) __attribute__((alias("lseek")));
static int sync_common(int fd, int data_only)
{
    int (*next)(int) = data_only ? next_fdatasync : next_fsync;
    if (!next) return syscall(data_only ? SYS_fdatasync : SYS_fsync, fd);
    if (!d_hook_enabled || fd < FD_FILE_BASE) return next(fd);
    pthread_mutex_lock(&lock_fd);
    struct file_obj *f = lookup(fd);
    int r = -EBADF;
    if (f) {
        fb_enter();
        r = fb_ops.fsync ? fb_ops.fsync(f->path, data_only, &f->fi) : -EOPNOTSUPP;
        fb_leave();
    }
    pthread_mutex_unlock(&lock_fd); return posix_result(r);
}
int fsync(int fd) { return sync_common(fd, 0); }
int fdatasync(int fd) { return sync_common(fd, 1); }
int close(int fd)
{
    if (!libc_close) return syscall(SYS_close, fd);
    if (!d_hook_enabled || fd < FD_FILE_BASE) return libc_close(fd);
    pthread_mutex_lock(&lock_fd);
    struct file_obj *f = lookup(fd);
    int r = -EBADF;
    if (f) {
        fb_enter();
        r = fb_ops.flush ? fb_ops.flush(f->path, &f->fi) : 0;
        if (fb_ops.release) fb_ops.release(f->path, &f->fi); /* FUSE ignores release's return value. */
        fb_leave();
        d_file_list[fd - FD_FILE_BASE] = NULL;
        free(f->path); free(f);
    }
    pthread_mutex_unlock(&lock_fd); return posix_result(r);
}

__attribute__((constructor)) static void initialize(void)
{
    fb_internal++;
#define RESOLVE(var, name) *(void **)(&var) = dlsym(RTLD_NEXT, name)
    RESOLVE(libc_open, "open64"); RESOLVE(libc_close, "close");
    RESOLVE(libc_read, "read"); RESOLVE(libc_write, "write");
    RESOLVE(next_pread, "pread64"); RESOLVE(next_pwrite, "pwrite64");
    RESOLVE(libc_lseek, "lseek64"); RESOLVE(next_fsync, "fsync"); RESOLVE(next_fdatasync, "fdatasync");
#undef RESOLVE
    const char *root = getenv("FUSE_BYPASS_MOUNT");
    if (!root) goto out;
    char *canonical = realpath(root, NULL);
    if (!canonical || !strcmp(canonical, "/") || strcmp(root, canonical)) {
        fprintf(stderr, "fuse-bypass: FUSE_BYPASS_MOUNT must be an existing canonical absolute directory, other than /\n");
        free(canonical); _exit(125);
    }
    struct stat st;
    if (stat(root, &st) || !S_ISDIR(st.st_mode)) { free(canonical); _exit(125); }
    dfs_list[0].fs_root = canonical;
    dfs_list[0].len_fs_root = strlen(canonical);
    num_dfs = 1;
    /* nr_open limits even a future raise of RLIMIT_NOFILE. */
    FILE *limits = fopen("/proc/sys/fs/nr_open", "r");
    unsigned long nr_open = ULONG_MAX;
    if (limits) { if (fscanf(limits, "%lu", &nr_open) != 1) nr_open = ULONG_MAX; fclose(limits); }
    if (nr_open > FD_FILE_BASE) config_error = EOVERFLOW;
    FILE *mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) config_error = errno;
    else {
        struct mntent *entry;
        while ((entry = getmntent(mounts))) {
            size_t n = dfs_list[0].len_fs_root;
            if (!strncmp(entry->mnt_dir, root, n) && entry->mnt_dir[n] == '/') {
                char **v = realloc(nested_mounts, (nested_count + 1) * sizeof(char *));
                if (!v) { config_error = ENOMEM; break; }
                nested_mounts = v;
                v[nested_count] = strdup(entry->mnt_dir);
                if (!v[nested_count]) { config_error = ENOMEM; break; }
                nested_count++;
            }
        }
        endmntent(mounts);
    }
    enabled = 1;
out:
    fb_internal--;
}
__attribute__((destructor)) static void finish(void)
{
    if (!enabled) return;
    for (int i = 0; i < MAX_OPENED_FILE; i++) if (d_file_list[i]) close(i + FD_FILE_BASE);
    fb_stop();
    fb_internal++;
    if (getenv("FUSE_BYPASS_STATS"))
        fprintf(stderr, "fuse-bypass: target_open=%lu read=%lu write=%lu forwarded_open=%lu\n",
                num_open, num_read, num_write, num_forward);
    free(dfs_list[0].fs_root);
    for (size_t i = 0; i < nested_count; i++) free(nested_mounts[i]);
    free(nested_mounts); enabled = 0;
    fb_internal--;
}
