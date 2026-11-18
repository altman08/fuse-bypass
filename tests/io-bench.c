#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts)) { perror("clock_gettime"); exit(1); }
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int cmp(const void *a, const void *b)
{
    double x=*(const double *)a, y=*(const double *)b;
    return (x>y)-(x<y);
}
static size_t number(const char *s, size_t max)
{
    char *end; errno=0;
    unsigned long long n=strtoull(s,&end,10);
    if (errno || !*s || *end || !n || n>max) { fprintf(stderr,"invalid number: %s\n",s); exit(2); }
    return (size_t)n;
}
int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr,"usage: %s FILE seqread|seqwrite|randread|randwrite|syncwrite BLOCK_BYTES OPERATIONS FILE_BLOCKS\n",argv[0]);
        return 2;
    }
    int writing = strstr(argv[2],"write") != NULL;
    int random = !strncmp(argv[2],"rand",4);
    int sync = !strcmp(argv[2],"syncwrite");
    if (strcmp(argv[2],"seqread") && strcmp(argv[2],"seqwrite") && strcmp(argv[2],"randread") && strcmp(argv[2],"randwrite") && !sync) return 2;
    size_t size=number(argv[3],16*1024*1024), count=number(argv[4],10000000), blocks=number(argv[5],1000000);
    char *buf=malloc(size);
    double *latency=malloc(count*sizeof(*latency));
    if (!buf || !latency) { perror("malloc"); return 1; }
    memset(buf,0x5a,size);
    int fd=open(argv[1],O_CREAT|O_TRUNC|O_RDWR,0600);
    if (fd<0) { perror("open"); return 1; }
    /* Identical setup in every mode; preparation excluded from timing. */
    for (size_t i=0;i<blocks;i++) {
        if (pwrite(fd,buf,size,(off_t)(i*size)) != (ssize_t)size) { perror("prepare"); return 1; }
    }
    if (fsync(fd)) { perror("prepare fsync"); return 1; }
    uint64_t rng=0x123456789abcdefULL;
    double cpu0=now(CLOCK_PROCESS_CPUTIME_ID), start=now(CLOCK_MONOTONIC);
    for (size_t i=0;i<count;i++) {
        rng ^= rng<<13; rng ^= rng>>7; rng ^= rng<<17;
        off_t off=(off_t)((random ? rng%blocks : i%blocks)*size);
        double t=now(CLOCK_MONOTONIC);
        ssize_t r=writing ? pwrite(fd,buf,size,off) : pread(fd,buf,size,off);
        if (r != (ssize_t)size) { fprintf(stderr,"short/error I/O: %zd errno=%d\n",r,errno); return 1; }
        if (sync && fsync(fd)) { perror("fsync"); return 1; }
        latency[i]=(now(CLOCK_MONOTONIC)-t)*1e6;
    }
    double seconds=now(CLOCK_MONOTONIC)-start, cpu=now(CLOCK_PROCESS_CPUTIME_ID)-cpu0;
    qsort(latency,count,sizeof(*latency),cmp);
    printf("mode,block_bytes,operations,file_blocks,seconds,MiB_per_s,IOPS,p50_us,p99_us,app_cpu_seconds\n");
    printf("%s,%zu,%zu,%zu,%.6f,%.3f,%.3f,%.3f,%.3f,%.6f\n",argv[2],size,count,blocks,seconds,
           (double)size*count/1048576/seconds,count/seconds,latency[(count-1)/2],latency[(count-1)*99/100],cpu);
    if (close(fd)) { perror("close"); return 1; }
    free(buf); free(latency); return 0;
}
