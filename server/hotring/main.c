#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <limits.h>

#include "test.h"

int main(int argc, char **argv)
{
	bool long_run = false;
	int opt;
	unsigned int seed = time(NULL);

	while ((opt = getopt(argc, argv, "ls:v")) != -1) {
		if (opt == 'l')
			long_run = true;
		else if (opt == 's')
			seed = strtoul(optarg, NULL, 0);
		else if (opt == 'v')
			test_verbose++;
	}

	printf("random seed %u\n", seed);
	srand(seed);

	printf("running tests\n");
	benchmark();

    exit(0);
}

