#define _GNU_SOURCE
#include "runtime.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern int fuse_bypass_target_main(int, char **);
__thread int fb_internal;
static __thread struct fuse_context context;
struct fuse_operations fb_ops;
static void *private_data;
static mode_t initial_umask;
static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int ready, stopping, started, start_error;
static pthread_once_t once = PTHREAD_ONCE_INIT;

void fb_enter(void)
{
    fb_internal++;
    memset(&context, 0, sizeof(context));
    context.uid = getuid(); context.gid = getgid(); context.pid = getpid();
    context.private_data = private_data;
    context.umask = initial_umask;
}
void fb_leave(void) { fb_internal--; }

struct fuse_context *fuse_get_context(void)
{
    if (fb_internal) return &context;
    struct fuse_context *(*next)(void) = dlsym(RTLD_NEXT, "fuse_get_context");
    return next ? next() : NULL;
}

/* --wrap applies only to the linked target code. The target's stack and argv
 * remain alive until all application handles have been released. */
int __wrap_fuse_main_real(int argc, char **argv, const struct fuse_operations *ops,
                          size_t size, void *data)
{
    (void)argc; (void)argv;
    if (!ops || size > sizeof(fb_ops)) {
        pthread_mutex_lock(&mutex);
        start_error = EPROTONOSUPPORT; ready = 1;
        pthread_cond_broadcast(&condition);
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    memcpy(&fb_ops, ops, size);
    private_data = data;
    fb_enter();
    /* There is no kernel session. Only connection-independent high-level
     * filesystems are supported. No capability is advertised. */
    struct fuse_conn_info conn = {0};
    struct fuse_config cfg = {0};
    conn.proto_major = 7; conn.proto_minor = 31;
    conn.max_write = conn.max_read = 1024 * 1024;
    if (fb_ops.init) private_data = fb_ops.init(&conn, &cfg);
    fb_leave();
    pthread_mutex_lock(&mutex);
    if (conn.want) start_error = EOPNOTSUPP;
    ready = 1;
    pthread_cond_broadcast(&condition);
    while (!stopping) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
    fb_enter();
    if (fb_ops.destroy) fb_ops.destroy(private_data);
    fb_leave();
    return start_error ? 1 : 0;
}

static void *run_target(void *unused)
{
    (void)unused;
    fb_internal = 1;
    char **args = calloc(258, sizeof(char *));
    int argc = 1;
    if (!args) goto fail;
    args[0] = strdup("fuse-bypass-target");
    if (!args[0]) goto fail;
    const char *file = getenv("FUSE_BYPASS_ARGS_FILE");
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f) goto fail;
        char *line = NULL; size_t cap = 0; ssize_t n;
        while ((n = getline(&line, &cap, f)) >= 0) {
            if (argc == 256) { free(line); fclose(f); goto fail; }
            if (n && line[n-1] == '\n') line[--n] = 0;
            args[argc] = strdup(line);
            if (!args[argc++]) { free(line); fclose(f); goto fail; }
        }
        int error = ferror(f);
        free(line); fclose(f);
        if (error) goto fail;
    } else {
        args[argc++] = strdup("-f");
        args[argc++] = strdup(getenv("FUSE_BYPASS_MOUNT"));
        if (!args[1] || !args[2]) goto fail;
    }
    /* User main may rewrite argv, so retain ownership separately. */
    char **owned = malloc((size_t)argc * sizeof(char *));
    if (!owned) goto fail;
    memcpy(owned, args, (size_t)argc * sizeof(char *));
    fuse_bypass_target_main(argc, args);
    for (int i = 0; i < argc; i++) free(owned[i]);
    free(owned); free(args);
    pthread_mutex_lock(&mutex);
    if (!ready) { start_error = EIO; ready = 1; pthread_cond_broadcast(&condition); }
    pthread_mutex_unlock(&mutex);
    return NULL;
fail:
    if (args) { for (int i = 0; i < argc; i++) free(args[i]); free(args); }
    pthread_mutex_lock(&mutex);
    start_error = EINVAL; ready = 1; pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    return NULL;
}
static void start_once(void)
{
    /* Read without changing the process-wide umask. Linux procfs is required. */
    int found_umask = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256]; unsigned mask;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "Umask:\t%o", &mask) == 1) { initial_umask = mask; found_umask = 1; break; }
        fclose(f);
    }
    if (!found_umask) { start_error = EOPNOTSUPP; return; }
    int r = pthread_create(&thread, NULL, run_target, NULL);
    if (r) { start_error = r; return; }
    started = 1;
    pthread_mutex_lock(&mutex);
    while (!ready) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
}
int fb_start(void)
{
    pthread_once(&once, start_once);
    return start_error ? -start_error : 0;
}
void fb_stop(void)
{
    if (!started) return;
    pthread_mutex_lock(&mutex);
    stopping = 1; pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    pthread_join(thread, NULL);
}
