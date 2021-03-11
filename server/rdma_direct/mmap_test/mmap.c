#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#define BUF_SIZE ((1UL << 30) * 1)

int main(int argc, char **argv)
{
    int fd;
    char *file = NULL;
    struct stat sb;
    int flag = PROT_WRITE | PROT_READ;

    if (argc < 2) {
        fprintf(stderr, "Usage: input\n");
        exit(1);
    }

    if ((fd = open(argv[1], O_RDWR|O_CREAT)) < 0) {
        perror("File Open Error");
        exit(1);
    }

    if (fstat(fd, &sb) < 0) {
        perror("fstat error");
        exit(1);
    }

    file = (char *)malloc(BUF_SIZE);
    file = (char *) mmap(0, BUF_SIZE, flag, MAP_SHARED, fd, 0);
    
    if (file == MAP_FAILED) {
        perror("mmap error");
        exit(1);
    }

    //printf("%s\n", file);
    memset(file, 0, BUF_SIZE);
    sleep(5);
    munmap(file, BUF_SIZE);
    close(fd);
}
