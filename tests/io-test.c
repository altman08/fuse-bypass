#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s (errno=%d %s)\n",__LINE__,#x,errno,strerror(errno)); exit(1); } } while (0)
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr,"usage: %s verify|write|expect|route PATH [text]\n",argv[0]); return 2; }
    const char *cmd = argv[1], *path = argv[2];
    if (!strcmp(cmd,"syncfail")) {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0x20000000);
        CHECK(fsync(fd) == -1 && errno == EOPNOTSUPP);
        CHECK(fdatasync(fd) == -1 && errno == EOPNOTSUPP);
        CHECK(close(fd) == 0); return 0;
    }
    if (!strcmp(cmd,"append")) {
        int fd = open(path, O_WRONLY | O_APPEND);
        CHECK(fd >= 0 && fd < 0x20000000);
        CHECK(write(fd,"!",1) == 1);
        CHECK(close(fd) == 0); return 0;
    }
    if (!strcmp(cmd,"route")) {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        printf("%s\n",fd >= 0x20000000 ? "bypass" : "kernel");
        CHECK(close(fd) == 0); return 0;
    }
    if (!strcmp(cmd,"write") || !strcmp(cmd,"expect")) {
        CHECK(argc == 4);
        int writing = !strcmp(cmd,"write");
        int fd = open(path, writing ? O_CREAT|O_TRUNC|O_WRONLY : O_RDONLY, 0600);
        CHECK(fd >= 0);
        size_t n = strlen(argv[3]);
        if (writing) { CHECK(write(fd,argv[3],n) == (ssize_t)n); CHECK(fsync(fd) == 0); }
        else { char b[256]; CHECK(n < sizeof(b)); CHECK(read(fd,b,sizeof(b)) == (ssize_t)n); CHECK(!memcmp(b,argv[3],n)); }
        CHECK(close(fd) == 0); return 0;
    }
    CHECK(!strcmp(cmd,"verify"));
    int fd = open(path, O_CREAT|O_TRUNC|O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd,"abcdef",6) == 6);
    CHECK(lseek(fd,0,SEEK_CUR) == 6);
    CHECK(pwrite(fd,"XY",2,2) == 2);
    CHECK(lseek(fd,0,SEEK_CUR) == 6);
    char b[32] = {0};
    CHECK(pread(fd,b,16,0) == 6 && !memcmp(b,"abXYef",6));
    CHECK(lseek(fd,0,SEEK_CUR) == 6);
    CHECK(read(fd,b,1) == 0);
    CHECK(lseek(fd,-2,SEEK_END) == 4);
    CHECK(read(fd,b,9) == 2 && !memcmp(b,"ef",2));
    CHECK(lseek(fd,-7,SEEK_SET) == -1 && errno == EINVAL);
    CHECK(pread(fd,b,1,-1) == -1 && errno == EINVAL);
    CHECK(read(fd,b,0) == 0 && write(fd,b,0) == 0);
    CHECK(fsync(fd) == 0 && fdatasync(fd) == 0);
    int second = open(path,O_RDONLY);
    CHECK(second >= 0 && second != fd);
    CHECK(read(second,b,2) == 2 && !memcmp(b,"ab",2));
    CHECK(lseek(fd,0,SEEK_CUR) == 6);
    CHECK(write(second,"!",1) == -1 && errno == EBADF);
    CHECK(close(second) == 0);
    CHECK(close(fd) == 0);
    CHECK(read(fd,b,1) == -1 && errno == EBADF);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(open(path,O_CREAT|O_EXCL|O_WRONLY,0600) == -1 && errno == EEXIST);
    fd = open(path,O_WRONLY|O_TRUNC);
    CHECK(fd >= 0);
    CHECK(read(fd,b,1) == -1 && errno == EBADF);
    CHECK(close(fd) == 0);
    fd = open(path,O_RDONLY);
    CHECK(fd >= 0 && read(fd,b,1) == 0 && close(fd) == 0);
    for (int i = 0; i < 3000; i++) { fd=open(path,O_RDONLY); CHECK(fd>=0); CHECK(close(fd)==0); }
    char missing[4096];
    CHECK(snprintf(missing,sizeof(missing),"%s.missing",path) < (int)sizeof(missing));
    CHECK(open(missing,O_RDONLY) == -1 && errno == ENOENT);
    puts("PASS"); return 0;
}
