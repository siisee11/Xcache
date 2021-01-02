#include <stdio.h>         // puts()
#include <stdlib.h>
#include <string.h>        // strlen()
#include <fcntl.h>         // O_WRONLY
#include <unistd.h>        // write(), close()
#include <time.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define  BUF_SIZE (1 << 20) // 1MB
#define TO_MB (1 << 20)

int ITER = 0;

typedef unsigned long long int UINT64;

UINT64 getRandom(UINT64 const min, UINT64 const max)
{
    return (((UINT64)(unsigned int)rand() << 32) + (UINT64)(unsigned int)rand()) % (max - min) + min;
}

enum READ_TYPE {
    SEQ_READ = 0,
    RAND_READ, 
    REVERSE_READ
};

static size_t get_file_size (const char * file_name)
{
    struct stat sb;
    if (stat (file_name, & sb) != 0) {
        fprintf (stderr, "'stat' failed for '%s': %s.\n",
                file_name, strerror (errno));
        exit (EXIT_FAILURE);
    }
    return sb.st_size;
}


void test_read(const char *_target_file, int type)
{
    const char *target_file =  _target_file;
    char     buf[BUF_SIZE];
    int      fd;
    ssize_t  rd_size;
    size_t totalRead = 0;

    size_t file_size = get_file_size(target_file);
    struct timeval start_time, end_time;
    double diff_time;

    printf("------ START ------\n");
    gettimeofday(&start_time, NULL);

    if ((fd = open(target_file, O_RDONLY)) > 0) {
        
        if (type == SEQ_READ) {
            ITER++;
            printf("\tSequential read..\n\n");
            while(0 < ( rd_size = read(fd, buf, sizeof(buf)))) {
                totalRead += rd_size;
                //if(totalRead % (1024 * sizeof(buf)) == 0)
                //    printf("\tRead %ld MB\n", totalRead/sizeof(buf));
            }
            printf("totalRead: %lu\n", totalRead);
            //printf("\tFINISH..\n\n");
            close(fd);
        }else if (type == RAND_READ) { 
            ITER++;
            printf("\tRandom read: %d\n\n", ITER);
            
            srand(time(NULL));
            while (file_size > totalRead) {
                off_t pos = getRandom(0, file_size);
                totalRead += pread(fd, buf, sizeof(buf), pos);
                //printf("\t\t pos: %ld\n", pos/TO_MB);
            }

            //printf("totalRead: %ld (MB)\n", totalRead/TO_MB);
            printf("\tFINISH..\n\n");
            close(fd);
        } else if (type == REVERSE_READ) {
            ITER++;
            printf("\tReverse read\n\n");
            off_t pos = file_size - sizeof(buf);
            while (file_size > totalRead) {
                totalRead += pread(fd, buf, sizeof(buf), pos);
                //printf("\t\t pos: %ld\n", pos/TO_MB);
                pos -= sizeof(buf);
            }

            //printf("totalRead: %ld (MB)\n", totalRead/TO_MB);
            printf("\tFINISH..\n\n");
            close(fd);

        }

    } else {
        fprintf(stderr, "\tFailed to open file...\n");
    }

    gettimeofday(&end_time, NULL);

    if(ITER == 1 && type == SEQ_READ) {
        diff_time = ((double)(end_time.tv_sec - start_time.tv_sec)*1000000 + 
                (end_time.tv_usec - start_time.tv_usec))/1000000;
        printf("SEQ_READ:\tExecution time: %.3f (s)\tBW:%.3f (MB/s)\n", diff_time, (double)(file_size/TO_MB)/diff_time);
        printf("------ END ------\n");
    }
    if(ITER == 1 && type == REVERSE_READ) {
        diff_time = ((double)(end_time.tv_sec - start_time.tv_sec)*1000000 + 
                (end_time.tv_usec - start_time.tv_usec))/1000000;
        printf("REVERSE_READ:\tExecution time: %.3f (s)\tBW:%.3f (MB/s)\n", diff_time, (double)(file_size/TO_MB)/diff_time);
        printf("------ END ------\n");
    }
    if(ITER == 1 && type == RAND_READ) {
        diff_time = ((double)(end_time.tv_sec - start_time.tv_sec)*1000000 + 
                (end_time.tv_usec - start_time.tv_usec))/1000000;

        printf("RAND_READ:\tExecution time: %.3f (s)\tBW:%.3f (MB/s)\n", diff_time, (double)(file_size/TO_MB)/diff_time);

        printf("------ END ------\n");
    }
    
}

void pass_pid_to_cgroup(void)
{
    int pid;
    char str_pid[8];
    
    pid = getpid();
    sprintf(str_pid, "%d", pid);

   // printf("pid: %s\n", str_pid);
    
    char cmd1[80] = "echo ";
    strcat(cmd1, str_pid);
    char cmd2[40] = " > /dev/cgroup/memory/test_process/tasks";
    strcat(cmd1, cmd2);
    printf("cmd: %s\n", cmd1);
    system(cmd1);
}

int main(int argc, char *argv[])
{
    pass_pid_to_cgroup();

    sleep(3);
    if(argc != 2) {
        fprintf(stderr, "Usage: %s <file_path>\n", argv[0]);
        exit(1);
    }

    const char *_target_file = argv[1];
    int i = 0;

    test_read(_target_file, SEQ_READ);    
    //test_read(_target_file, REVERSE_READ);    
    //test_read(_target_file, RAND_READ);    
    //test_read(_target_file, RAND_READ);    

    return 0;
}
