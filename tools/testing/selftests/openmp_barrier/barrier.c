#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#define TV2NS(tv)	((tv).tv_sec * 1000000000L + (tv).tv_usec * 1000L)

void work(void)
{
	int i;
	volatile int *p = &i;
	for (*p = 0; *p < 1000000; (*p)++)
		;
}

int main(int argc, char *argv[])
{
	struct timeval t1, t2;
	long i, j, elapsed;
	int n = (argc > 1 ? atoi(argv[1]) : 1000);
	int nt = atoi(getenv("OMP_NUM_THREADS"));
	gettimeofday(&t1, 0);

	for (i = 0; i < n; i++)
		#pragma omp parallel for
		for (j = 0; j < nt; j++)
			work();
	gettimeofday(&t2, 0);
	elapsed = TV2NS(t2) - TV2NS(t1);
	printf("%.2f iters/sec\n", n / (elapsed / 1e9));
}
