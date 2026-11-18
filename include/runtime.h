#ifndef FB_RUNTIME_H
#define FB_RUNTIME_H
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#include <fuse.h>
extern __thread int fb_internal;
extern struct fuse_operations fb_ops;
int fb_start(void);
void fb_stop(void);
void fb_enter(void);
void fb_leave(void);
#endif
