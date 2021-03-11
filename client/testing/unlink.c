i#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#define BUFF_SIZE 1*1024*1024
int fd;
char *target_file = "/home/daegyu/dummy/my_1G_file";

int read_file()
{
	char     buff[BUFF_SIZE];
	int      fd;
	ssize_t  rd_size;
	unsigned long totalRead = 0;

	if (0 < (fd = open(target_file, O_RDONLY))) {
		while( 0 < ( rd_size = read(fd, buff, BUFF_SIZE)))
		{
			totalRead += rd_size;

			if(totalRead % (100 * BUFF_SIZE) == 0)
				printf("\tRead %ld MB\n", totalRead);
		}

		printf("\tFINISH..\n");
		close(fd);
		return 0;
	} else {
		printf("\tFailed to open file...\n");
		return -1;
	}
}

int unlink_file()
{
	if(unlink(target_file) == -1) {
		perror("delete failed!");
		return 1;
	}
}

int main()
{
	read_file();
	sleep(1);
	unlink_file();

    /*
	if(remove("remove.txt") == -1) {		
		perror("remove.txt delete failed!");
		return 1;
	}
    */
}
