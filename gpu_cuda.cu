#include <iostream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>

#include "common.h"

#define L2_CACHE_SIZE           (2 * 1024 * 1024)
#define L2_EVICT_READ_SIZE      (L2_CACHE_SIZE)
#define L2_MIN_STRIDE           (128)

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort=true)
{
   if (code != cudaSuccess)
   {
      fprintf(stderr,"GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
      if (abort) exit(code);
   }
}

__device__
uint64_t refresh(volatile uint64_t *refresh_vaddr)
{
    uint64_t curindex = 0;
    uint64_t sum = 0;

    while (curindex != (uint64_t)-1) {
        curindex = refresh_vaddr[curindex];
        sum += curindex;
    }
    return sum;
}

__global__
void read_pair(volatile uint64_t *a_v, volatile uint64_t *b_v,
        volatile uint64_t *refresh_v, volatile double *ticks, volatile uint64_t *psum,
        double threshold)
{
    uint64_t curindex;
    uint64_t sum;
    uint64_t count;
    uint64_t mid;
    uint64_t previndex;
    const uint64_t sharednum = MIN_BANK_SIZE/(sizeof(uint64_t));
    __shared__ uint64_t s[sharednum];
    __shared__ uint64_t t[sharednum];
    uint64_t tsum;
    int i;
    double tick;

    for (i = 0; i < MAX_OUTER_LOOP + 1; i++) {

        sum = 0;
        curindex = 0;

        /* Refresh the L2 cache */
        sum += refresh(refresh_v);

        while (curindex != (uint64_t)-1) {
            previndex = curindex;
            mid = clock64();
            sum += b_v[curindex];
            curindex = a_v[curindex];
            s[previndex] = curindex;
            t[previndex] = clock64() - mid;
        }
    
        curindex = 0;
        tsum = 0;
        count = 0;
        while (curindex != (uint64_t)-1) {
            count++;
            tsum += t[curindex];
            //printf("Ticks: %ld, Index: %ld\n", t[curindex], curindex);
            curindex = s[curindex];
        }
       
        /* First run is warmup */
	    if (i == 0)
    	    continue;

        tick = ((double)tsum) / ((double)count);
        if (tick > threshold) {
            /* We don't expect threshold to be crossed on GPU */
            printf("ERROR: Threshold:%f, Ticks:%f, i:%d, count: %ld\n", threshold, tick, i, count);
            i--;
            continue;
        }

        ticks[i - 1] = tick;
        psum[i - 1] = sum;
    }
}

void shuffle(uint64_t *array, size_t n)
{
    size_t i;
    for (i = 0; i < n - 1; i++) 
    {
        size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
        uint64_t t = array[j];
        array[j] = array[i];
        array[i] = t;
    }
}

/* 
 * Initialize the pointer chase for refresh vaddr to hinder an hardware
 * mechanism to predict access pattern
 */
void init_pointer_chase(uint64_t *array, size_t size, size_t offset, int do_shuffle)
{
    uint64_t num_elem = size / offset;
    uint64_t *indexes = (uint64_t *)malloc(size);
    uint64_t curindex;
    uint64_t i;

    assert(offset >= sizeof(uint64_t));
    assert((offset % sizeof(uint64_t)) == 0);
    assert(indexes != NULL);

    for (i = 0; i < num_elem; i++) {
        indexes[i] = i;
    }

    if (do_shuffle)
    	shuffle(indexes, num_elem);

    for (i = 0, curindex = 0; i < num_elem; i++) {
        array[curindex] = indexes[i] * (offset / sizeof(uint64_t));
        curindex = indexes[i] * (offset / sizeof(uint64_t));
    }

    array[curindex] = (uint64_t)-1;

    free(indexes);
}

extern "C" {

void *allocate_gpu_contigous(int contiguous_pages, uintptr_t *phy_start)
{
    size_t size = contiguous_pages * PAGE_SIZE;
    void *gpu_mem;
    int device = -1;

    gpuErrchk(cudaMallocManaged(&gpu_mem, size));
   
    /* To print out dmesg */
    gpuErrchk(cudaGetDevice(&device));
    gpuErrchk(cudaMemPrefetchAsync(gpu_mem, size, device, NULL)); 

    printf("Enter the physical start address:\n");
    scanf("0x%lx", phy_start);
    printf("User input is: 0x%lx\n", *phy_start);

    return gpu_mem;
}

double find_gpu_read_time(void *_a, void *_b, double threshold)
{
    uint64_t *a = (uint64_t *)_a;
    uint64_t *b = (uint64_t *)_b;
    int i;
    double min_ticks, max_ticks, sum_ticks;
    double avg_ticks;
    int device = -1;
    static uint64_t *sum, *refresh_v;
    static double *ticks;
    static int is_initialized = 0;
    static uint64_t *start_v;

    if (is_initialized == 0) {
        gpuErrchk(cudaGetDevice(&device));
        gpuErrchk(cudaMallocManaged(&refresh_v, L2_EVICT_READ_SIZE));
        gpuErrchk(cudaMallocManaged(&ticks, (MAX_OUTER_LOOP) * sizeof(double)));
        gpuErrchk(cudaMallocManaged(&sum, (MAX_OUTER_LOOP) * sizeof(uint64_t)));
        
        /* TODO: Why does this whole thing become slow then I make 'true' -> 'false' */
        init_pointer_chase(refresh_v, L2_EVICT_READ_SIZE, sizeof(uint64_t), true);
        start_v = a;

        is_initialized = true;
    }

    init_pointer_chase(a, MIN_BANK_SIZE, L2_MIN_STRIDE, false); 
    gpuErrchk(cudaMemPrefetchAsync(start_v, MEM_SIZE, device, NULL));
    gpuErrchk(cudaMemPrefetchAsync(refresh_v, L2_EVICT_READ_SIZE, device, NULL));


    gpuErrchk(cudaDeviceSynchronize());
    read_pair<<<1,1>>>(a, b, refresh_v, ticks, sum, threshold);
    gpuErrchk(cudaPeekAtLastError());
    gpuErrchk(cudaDeviceSynchronize()); 

    for (i = 0, min_ticks = LONG_MAX, sum_ticks = 0, max_ticks = 0; 
            i < MAX_OUTER_LOOP; i++) {
        double tick = ticks[i];

        assert(tick > 0);
		
        min_ticks = tick < min_ticks ? tick : min_ticks;
        max_ticks = tick > max_ticks ? tick : max_ticks;
        sum_ticks += tick;
    }

    avg_ticks = (sum_ticks * 1.0f) / MAX_OUTER_LOOP;
    dprintf("Avg Ticks: %0.3f,\tMax Ticks: %0.3f,\tMin Ticks: %0.3f\n",
            avg_ticks, max_ticks, min_ticks);
    return avg_ticks;
    
}

} // extern "C"
