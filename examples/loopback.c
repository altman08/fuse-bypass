#define _GNU_SOURCE
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A small target FUSE filesystem for integration testing. Only flat regular
 * files are supported. This file builds unchanged as a daemon or into fuse-bypass.so. */
static int root = -1;
static unsigned long reads, writes, opens;
static int name_of(const char *path, const char **name)
{
    struct fuse_context *ctx = fuse_get_context();
    if (!ctx || ctx->private_data != &root) return -EIO;
    if (!path || path[0] != '/') return -EINVAL;
    *name = path + 1;
    if (!**name) { *name = "."; return 0; }
    if (strchr(*name, '/') || !strcmp(*name, ".") || !strcmp(*name, ".."))
        return -EOPNOTSUPP;
    return 0;
}
static int start(const char *config, void **data)
{
    if (config[0] != '/') return -EINVAL;
    root = open(config, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) return -errno;
    *data = &root;
    return 0;
}
static void destroy(void *data)
{
    (void)data;
    if (getenv("FUSE_BYPASS_STATS"))
        fprintf(stderr, "adapter: open=%lu read=%lu write=%lu\n", opens, reads, writes);
    close(root);
    root = -1;
}
static void *init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->direct_io = 1;
    cfg->entry_timeout = cfg->attr_timeout = cfg->negative_timeout = 0;
    return fuse_get_context()->private_data;
}
static int attr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    const char *name;
    int r = name_of(path, &name);
    if (r) return r;
    r = fi ? fstat((int)fi->fh, st) : fstatat(root, name, st, AT_SYMLINK_NOFOLLOW);
    return r < 0 ? -errno : 0;
}
static int do_open(const char *path, mode_t mode, struct fuse_file_info *fi, int create)
{
    const char *name;
    int r = name_of(path, &name);
    if (r) return r;
    int fd = openat(root, name, fi->flags | O_NOFOLLOW | O_CLOEXEC | (create ? O_CREAT : 0), mode);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) { r = -errno; close(fd); return r; }
    if (!S_ISREG(st.st_mode)) { close(fd); return -EOPNOTSUPP; }
    fi->fh = (uint64_t)fd;
    fi->direct_io = 1;
    opens++;
    return 0;
}
static int file_open(const char *p, struct fuse_file_info *fi) { return do_open(p, 0, fi, 0); }
static int create(const char *p, mode_t m, struct fuse_file_info *fi) { return do_open(p, m, fi, 1); }
static int release(const char *p, struct fuse_file_info *fi)
{
    (void)p;
    return close((int)fi->fh) < 0 ? -errno : 0;
}
static int read_file(const char *p, char *buf, size_t n, off_t off, struct fuse_file_info *fi)
{
    (void)p;
    reads++;
    ssize_t r = pread((int)fi->fh, buf, n, off);
    return r < 0 ? -errno : (int)r;
}
static int write_file(const char *p, const char *buf, size_t n, off_t off, struct fuse_file_info *fi)
{
    (void)p;
    writes++;
    ssize_t r = pwrite((int)fi->fh, buf, n, off);
    return r < 0 ? -errno : (int)r;
}
static int truncate_file(const char *p, off_t size, struct fuse_file_info *fi)
{
    int fd, r;
    const char *name;
    r = name_of(p, &name);
    if (r) return r;
    if (fi) fd = (int)fi->fh;
    else {
        fd = openat(root, name, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -errno;
    }
    r = ftruncate(fd, size) < 0 ? -errno : 0;
    if (!fi) close(fd);
    return r;
}
static int sync_file(const char *p, int data_only, struct fuse_file_info *fi)
{
    (void)p;
    int r = data_only ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
    return r < 0 ? -errno : 0;
}
static int list(const char *p, void *buf, fuse_fill_dir_t fill, off_t off,
                struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)off; (void)fi; (void)flags;
    if (strcmp(p, "/")) return -ENOTDIR;
    int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    DIR *d = fdopendir(fd);
    if (!d) { int e = errno; close(fd); return -e; }
    struct dirent *ent;
    while ((ent = readdir(d))) if (fill(buf, ent->d_name, NULL, 0, 0)) break;
    closedir(d);
    return 0;
}
static const struct fuse_operations ops = {
    .init = init, .destroy = destroy, .getattr = attr, .open = file_open, .create = create,
    .release = release, .read = read_file, .write = write_file,
    .truncate = truncate_file, .fsync = sync_file, .readdir = list
};
/* Ordinary libfuse3 source: no fuse-bypass header or registration API. */
int main(int argc, char **argv)
{
    void *data = NULL;
    const char *backing = getenv("DEMO_BACKING");
    int r = backing ? start(backing, &data) : -EINVAL;
    if (r) {
        fprintf(stderr, "Set DEMO_BACKING to an absolute storage directory: %s\n", strerror(-r));
        return 1;
    }
    r = fuse_main(argc, argv, &ops, data);
    /* FUSE destroy normally closed root. Also clean up if startup failed. */
    if (root >= 0) close(root);
    return r;
}
