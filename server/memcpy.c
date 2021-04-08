#include <time.h>
#include <cstring>
#include <stdio.h>

#define SIZE 4096
#define USE_MEMCPY
#define ITER 1000


int main(void)
{
	struct timespec start, end;
	long long unsigned int elapsed=0;

	char a[ITER][SIZE];
	char b[ITER][SIZE];
	int n;

	/* code 'filling' a[] */
	clock_gettime(CLOCK_MONOTONIC, &start);

	for ( int i = 0 ; i < ITER; i++) {
#ifdef USE_MEMCPY
		memcpy(b[i], a[i], SIZE);
#else
		for (n = 0; n < sizeof(a); n++)
		{
			b[n] = a[n];
		}
#endif
	}

	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed+= end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);

	printf("elapsed=%llu nsec\n", elapsed/ITER);
}
