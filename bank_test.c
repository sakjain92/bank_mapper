#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/sysinfo.h>
#include <sched.h>

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

#define MEM_SIZE                (1 << 22)
#define NUM_CONTIGOUS_PAGES     5
#define MAX_MMAP_ITR            100

#define MAX_INNER_LOOP  10
#define MAX_OUTER_LOOP  1000000

#define THRESHOLD_MULTIPLIER    5

// CORE to run on : -1 for last processor
#define CORE    -1
#define IA32_MISC_ENABLE_OFFSET 0x1a4
#define DISBALE_PREFETCH(msr)   (msr |= 0xf)

// On some systems, HW prefetch details are not well know. Use BIOS setting for
// disabling it
#define SOFTWARE_CONTROL_HWPREFETCH 1

#if (SOFTWARE_CONTROL_HWPREFETCH == 1)    
static int disable_prefetch(int *core, uint64_t *flag)
{
    // Assocaite with a single processor
    int num_cpu = get_nprocs();
    int cpu = CORE;
    cpu_set_t  mask;
    int ret, fd;
    char fname[100];
    uint64_t msr;

    if (cpu == -1) {
        cpu = num_cpu - 1;
    } else if (cpu >= num_cpu) {
        fprintf(stderr, "Invalid CORE\n");
        return -1;
    }
    
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    CPU_SET(cpu, &mask);

    ret = sched_setaffinity(0, sizeof(mask), &mask);
    if (ret < 0) {
        fprintf(stderr, "Couldn't set the process affinity\n");
        return -1;
    }
    
    *core = cpu;
    printf("Running on core %d\n", cpu);

    // See: https://software.intel.com/en-us/articles/disclosure-of-hw-prefetcher-control-on-some-intel-processors
    // For details on how to disable prefetching
    // Cross-checked with intel mlc tool and by seeing effect of disabling prefetching
    // via BIOS on i7-700

    sprintf(fname, "/dev/cpu/%d/msr", cpu);
    ret = fd = open(fname, O_RDWR);
    if (ret < 0) {
        fprintf(stderr, "Couldn't open msr dev. Please run 'modprobe msr' and run this program with root permissions\n");
        return -1;
    }
 
    ret = pread(fd, &msr, sizeof(msr), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't read msr dev\n");
        return -1;
    }

    *flag = msr;
    printf("Old MSR:0x%lx\n", msr);

    DISBALE_PREFETCH(msr);

    ret = pwrite(fd, &msr, sizeof(msr), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't write msr dev: %s\n", strerror(errno));
        return -1;
    }

    printf("New MSR:0x%lx\n", msr);

    return 0;
}

static int enable_prefetch(int core, uint64_t flag)
{
    int ret, fd;
    char fname[100];
    uint64_t msr;

    sprintf(fname, "/dev/cpu/%d/msr", core);
    ret = fd = open(fname, O_RDWR);
    if (ret < 0) {
        fprintf(stderr, "Couldn't open msr dev. Please run 'modprobe msr'\n");
        return -1;
    }
 
    ret = pread(fd, &msr, sizeof(msr), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't read msr dev\n");
        return -1;
    }

    printf("New MSR:0x%lx\n", msr);

    ret = pwrite(fd, &flag, sizeof(flag), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't write msr dev\n");
        return -1;
    }

    printf("Set MSR:0x%lx\n", flag);

    return 0;
}
#endif /* SOFTWARE_CONTROL_HWPREFETCH == 1 */

static inline uint64_t currentTicks(void)
{
      unsigned int a, d;
      asm volatile("rdtsc" : "=a" (a), "=d" (d));
      return (uint64_t)(a) | ((uint64_t)(d) << 32);
}

// Returns the avg time
double find_read_time(void *_a, void *_b, double threshold)
{
    uint32_t a = (uint32_t)(uintptr_t)_a;
    uint32_t b = (uint32_t)(uintptr_t)_b;
    int i, j;
    uint64_t start_ticks, end_ticks, ticks;
    uint64_t min_ticks, max_ticks, sum_ticks;
    double avg_ticks;
    int sum;

    assert((uintptr_t)(a) == (uintptr_t)(_a));
    assert((uintptr_t)(b) == (uintptr_t)(_b));

    *(uint64_t *)(_a) = 0;
    *(uint64_t *)(_b) = 0;
    for (i = 0, sum_ticks = 0, min_ticks = LONG_MAX, max_ticks = 0;
            i < MAX_OUTER_LOOP; i++) {
        
        start_ticks = currentTicks();
        for (j = 0, sum = 0; j < MAX_INNER_LOOP; j++) {
            asm volatile ("addl (%1), %0\n\t"
                          "clflush (%1)\n\t"
                          "mfence\n\t"
                          "addl (%2), %0\n\t"
                          "clflush (%2)\n\t"
                          "mfence\n\t": "=r" (sum) : "r" (a), "r" (b) : "memory");
        }
        end_ticks = currentTicks();
        
        ticks = end_ticks - start_ticks;
        assert(ticks > 0);
        assert(*(uint64_t *)_a == 0);
        assert(*(uint64_t *)_b == 0);
        // TODO: Why is sum not zero?
        //if (sum != 0)
        //    printf("Sum is:%d\n", sum);
        //assert(sum == 0);

        /* As there are timer interrupts, we reject outliers based on threshold */
        if ((double)(ticks) > THRESHOLD_MULTIPLIER * threshold) {
            i--;
            continue;
        }
        min_ticks = ticks < min_ticks ? ticks : min_ticks;
        max_ticks = ticks > max_ticks ? ticks : max_ticks;
        sum_ticks += ticks;
    }

    avg_ticks = (sum_ticks * 1.0f) / MAX_OUTER_LOOP;
    printf("Avg Ticks: %0.3f,\tMax Ticks: %ld,\tMin Ticks: %ld\n",
            avg_ticks, max_ticks, min_ticks);
    return avg_ticks;
}

uintptr_t get_physical_addr(uintptr_t virtual_addr) {
    
    uint64_t frame_num;
    int ret;
    uint64_t value;
    off_t pos;
    int fd = open("/proc/self/pagemap", O_RDONLY);
    assert(fd >= 0);
    
    pos = lseek(fd, (virtual_addr / PAGE_SIZE) * 8, SEEK_SET);
    assert(pos >= 0);
    
    ret = read(fd, &value, 8);
    assert(ret == 8);
    
    ret = close(fd);
    assert(ret == 0);
    
    frame_num = value & ((1ULL << 54) - 1);
    return (frame_num * PAGE_SIZE) | (virtual_addr & PAGE_MASK);
}

/* Searches if from start to start + length - 1 has at any point contigous pages
 * which are contigous. If so, returns the start address of them
 */
void *is_contiguous(void *_start, size_t length, int contigous_pages)
{
    uintptr_t start = (uintptr_t)_start;
    uintptr_t end = start + length - 1;
    uintptr_t current;
    uintptr_t prev_phy_addr;
    int found = 1;

    assert((start & PAGE_MASK) == 0);
    assert((length & PAGE_MASK) == 0);
    assert((start + length) > start);

    for(current = start + PAGE_SIZE, prev_phy_addr = get_physical_addr(start);
            current <= end && found < contigous_pages;
            current += PAGE_SIZE) {
        
        uintptr_t cur_phy_addr = get_physical_addr(current);
        if (cur_phy_addr == (prev_phy_addr + PAGE_SIZE)) {
                found++;
        } else {
            start = current;
            found = 1;
        }

        prev_phy_addr = cur_phy_addr;
    } 

    if (found >= contigous_pages) {
       
        printf("Found contiguous pages\n");
        for (int i = 0; i < found; i++) {
            uintptr_t v = start + i * PAGE_SIZE;
            printf("Virt:0x%lx, Phy:0x%lx\n", v,  get_physical_addr(v));
        }
        return (void *)start;
    }

    return NULL;
}

/* Tries to allocate physical contigous pages and return the start address */
void *allocate_contigous(int contiguous_pages) {
    
    int max_itr = MAX_MMAP_ITR;
    int i;
    void *ret;
    size_t len = MEM_SIZE;

    for (i = 0; i < max_itr; i++) { 
        void *start = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                           -1, 0);
        assert(start != MAP_FAILED);
        assert(mlock(start, len) == 0);

        ret = is_contiguous(start, len, contiguous_pages);
        if (ret != NULL) {
            return ret;
        }

        assert(munlock(start, len) == 0);
        assert(munmap(start, len) == 0);
    }

    return NULL;
}

int main()
{
    void *start;
    uintptr_t a, b;
    float threshold = LONG_MAX;

#if (SOFTWARE_CONTROL_HWPREFETCH == 1)
    int ret;
    uint64_t pflag;
    int core;
#endif

    // TODO: Install sigsegv handler
    printf("This program needs root permissions and currently only supports x86/x86-64\n");
    printf("Please don't terminate the program by Ctrl-C\n");

#if (SOFTWARE_CONTROL_HWPREFETCH == 1)
    ret = disable_prefetch(&core, &pflag);
    if (ret < 0) {
        fprintf(stderr, "Couldn't disable prefetch\n");
        return -1;
    }
#endif

    start = allocate_contigous(NUM_CONTIGOUS_PAGES);
    if (start == NULL) {
        fprintf(stderr, "Couldn't find the physical contiguous addresses\n");
        return -1;
    }

    // Warm up 
    a = (uintptr_t)start;
    b = a + sizeof(uint64_t);
    threshold = find_read_time((void *)a, (void *)b, threshold);

    printf("Threshold is %f\n", THRESHOLD_MULTIPLIER * threshold);
    
    printf("Same page:\n");
    a = (uintptr_t)start;
    b = a + sizeof(uint64_t) + 0x100;
    find_read_time((void *)a, (void *)b, threshold);

    printf("Across 1 pages:\n");
    a = (uintptr_t)start;
    b = a + sizeof(uint64_t) + 1 * PAGE_SIZE;
    find_read_time((void *)a, (void *)b, threshold);

    printf("Across 2 pages:\n");
    a = (uintptr_t)start;
    b = a + sizeof(uint64_t) + 2 * PAGE_SIZE;
    find_read_time((void *)a, (void *)b, threshold);

    printf("Across 4 pages:\n");
    a = (uintptr_t)start;
    b = a + sizeof(uint64_t) + 4 * PAGE_SIZE;
    find_read_time((void *)a, (void *)b, threshold);

#if (SOFTWARE_CONTROL_HWPREFETCH == 1)
    ret = enable_prefetch(core, pflag);
    if (ret < 0) {
        fprintf(stderr, "ERROR:Couldn't reset prefetching\n");
        return -1;
    }
#endif
    return 0;
}
