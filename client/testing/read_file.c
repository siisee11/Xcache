#include <stdio.h>         // puts()
#include <stdlib.h>
#include <string.h>        // strlen()
#include <fcntl.h>         // O_WRONLY
#include <unistd.h>        // write(), close()
#include <time.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/stat.h>

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
	RAND_READ
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
			printf("\tSequential read..\n\n");
			while(0 < ( rd_size = read(fd, buf, sizeof(buf)))) {
				totalRead += rd_size;
				//if(totalRead % (1024 * sizeof(buf)) == 0)
				//    printf("\tRead %ld MB\n", totalRead/sizeof(buf));
			}
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
		}

	} else {
		fprintf(stderr, "\tFailed to open file...\n");
	}

	gettimeofday(&end_time, NULL);

	if(ITER == 1 && type == RAND_READ) {
		diff_time = ((double)(end_time.tv_sec - start_time.tv_sec)*1000000 + 
				(end_time.tv_usec - start_time.tv_usec))/1000000;

		printf("RAND_READ:\tExecution time: %.3f (s)\tBW:%.3f (MB/s)\n", diff_time, (double)(file_size/TO_MB)/diff_time);

		printf("------ END ------\n");
	}

}


int main(int argc, char *argv[])
{
	if(argc != 2) {
		fprintf(stderr, "Usage: %s <file_path>\n", argv[0]);
		exit(1);
	}

	const char *_target_file = argv[1];
	int i = 0;

	//test_read(_target_file, SEQ_READ);    
	//test_read(_target_file, RAND_READ);    
	test_read(_target_file, RAND_READ);    

	return 0;
}

