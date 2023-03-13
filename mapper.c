#include <bits/types/struct_timeval.h>
#include <hwloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

#define CLEARLINE()    do{ fprintf(stderr, "\033[2K\033[1F\033[2K"); }while(0)

#define LOG(...)       do{ fprintf(stderr, "%s:%d -- ", __FUNCTION__, __LINE__); \
		           fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }while(0)


double __get_ts(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + tv.tv_usec * 1e-6;
}

static inline int __bind_thread_to(hwloc_topology_t topology, hwloc_obj_t core)
{
	int ret = hwloc_set_cpubind(topology, core->cpuset, HWLOC_CPUBIND_THREAD);

	assert(ret != -1);
	return ret;
}

struct alloc_params_s
{
	hwloc_topology_t topology;
	size_t           size;
	hwloc_obj_t      core;
	void *           ret_addr;
};

static inline size_t __round_size(size_t size)
{
	return ( (size / getpagesize() ) + 1) * getpagesize();
}

static inline void *map(size_t size)
{
	void *ret = mmap(NULL, __round_size(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if( (long int)ret == -1)
	{
		perror("mmap");
		return NULL;
	}

	size_t offset = 0;

	for(offset = 0; offset < size; offset++)
	{
		( (char *)ret)[offset] = offset;
	}

	return ret;
}

static inline void unmap(void *addr, size_t size)
{
	munmap(addr, __round_size(size) );
}

static inline void *__do_alloc(void *p_args)
{
	struct alloc_params_s *args = (struct alloc_params_s *)p_args;

	if(__bind_thread_to(args->topology, args->core) < 0)
	{
		LOG("Failed to bind thread");
		exit(1);
	}

	LOG("Allocating %g GB on core", args->size / (1024.0 * 1024.0 * 1024.0) );

	void *ret = map(args->size);

	args->ret_addr = ret;

	return NULL;
}

void *allocate_on_core(hwloc_topology_t topology, size_t size, int core_idx)
{
	assert(core_idx < hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE) );
	hwloc_obj_t t_core = hwloc_get_obj_by_type(topology, HWLOC_OBJ_CORE, core_idx);

	struct alloc_params_s p;

	p.core     = t_core;
	p.topology = topology;
	p.size     = size;
	p.ret_addr = NULL;

	pthread_t th;

	pthread_create(&th, NULL, __do_alloc, &p);
	pthread_join(th, NULL);

	return p.ret_addr;
}

unsigned int average_passes = 100;

double compute_read_bw(void *target, size_t size)
{
	/* Allocate local buff and generate faults */
	void *local_buff = map(size);

	if(!local_buff)
	{
		perror("malloc");
		return 0.0;
	}

	unsigned int cnt = 0;

	double total_time = 0.0;

	LOG("Measurement %d / %u", 0, average_passes);

	do
	{
		CLEARLINE();
		LOG("Measurement %d / %u", cnt, average_passes);
		memset(local_buff, 0, size);

		double start = __get_ts();
		memcpy(local_buff, target, size);
		double end = __get_ts();

		total_time += end - start;

		cnt++;
	}while(cnt < average_passes);

	unmap(local_buff, size);

	return (double)size * average_passes / total_time;
}

static inline void __show_help(char **argv)
{
    fprintf(stderr, "%s -s [SIZE] -i [ITER] -o [OUTPUT JSON]\n", argv[0]);
    fprintf(stderr, "-s : total size to move in bytes\n");
    fprintf(stderr, "-i : number of averaging iterations\n");
    fprintf(stderr, "-o : output json file\n");

	exit(0);
}


int main(int argc, char **argv)
{
	int    opt;
	size_t size = 100 * 1024 * 1024;

	char *out = NULL;

	while( (opt = getopt(argc, argv, "hs:i:o:") ) != -1)
	{
		char *dummy;

		switch(opt)
		{
			default: /* '?' */
			case 'h':
				__show_help(argv);
				break;

			case 'o':
				out = strdup(optarg);
				break;

			case 'i':
				average_passes = strtol(optarg, &dummy, 10);
				LOG("Setting average passes to %u", average_passes);
				break;

			case 's':
				size = strtol(optarg, &dummy, 10);
				LOG("Setting data size to %ld", size);
				break;
		}
	}

	FILE *fdout = NULL;

	if(out)
	{
		fdout = fopen(out, "w");

		if(!fdout)
		{
			perror("fopen");
			return 1;
		}

		fprintf(fdout, "{");
	}


	hwloc_topology_t topology;
	int nbcores;

	hwloc_topology_init(&topology);
	hwloc_topology_load(topology);

	nbcores = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
	assert(nbcores > 0);

	int j;

	for(j = 0; j < nbcores; j++)
	{
		if(fdout)
		{
			fprintf(fdout, "\"%d\" : {\n", j);
		}

		hwloc_obj_t t_core = hwloc_get_obj_by_type(topology, HWLOC_OBJ_CORE, j);

		if(__bind_thread_to(topology, t_core) < 0)
		{
			printf("Failed to bind root thread\n");
			return 1;
		}

		int i;

		for(i = 0; i < nbcores; i++)
		{
			void *mem = allocate_on_core(topology, size, i);

			double bw = compute_read_bw(mem, size);

			LOG("Bandwidth %d - %d is %g GB/sec", j, i, bw / (1024.0 * 1024.0 * 1024.0) );

			if(fdout)
			{
				fprintf(fdout, "\"%d\" : %g%s\n", i, bw, i < (nbcores - 1)?",":"");
			}

			unmap(mem, size);
		}

		if(fdout)
		{
			fprintf(fdout, "}%s\n", j < (nbcores - 1)?",":"");
		}
	}

	if(fdout)
	{
		fprintf(fdout, "}");
		fclose(fdout);
		free(out);
	}

	hwloc_topology_destroy(topology);

	return 0;
}
