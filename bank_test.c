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
#include <stdbool.h>
#include <sys/ioctl.h>

#define DEBUG
#ifdef DEBUG
#define dprintf(...)    printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

#define PAGE_SHIFT  12
#define PAGE_SIZE   (1 << PAGE_SHIFT)
#define PAGE_MASK   (PAGE_SIZE - 1)

// Are we using kernel allocator module to allocate contiguous memory?
#define KERNEL_ALLOCATOR_MODULE 1
#define KERNEL_ALLOCATOR_MODULE_FILE    "/dev/kam"

#define MEM_SIZE                (1 << 14)

// Using mmap(), we might/might not get contigous pages. We need to try multiple
// times.
// Using kernel module, if we can get, we wll get all contiguous on first attempt
#if (KERNEL_ALLOCATOR_MODULE == 1)
#define NUM_CONTIGOUS_PAGES     (MEM_SIZE / PAGE_SIZE)
#define MAX_MMAP_ITR            1
#else
#define NUM_CONTIGOUS_PAGES     5
#define MAX_MMAP_ITR            10
#endif

#define MAX_INNER_LOOP  10
#define MAX_OUTER_LOOP  100000

// Threshold for timing
#define THRESHOLD_MULTIPLIER    5

// By what percentage does a timing needs to be away from average to be considered
// outlier and hence we can assume that pair of address lie on same bank, different
// rows
#define OUTLIER_PERCENTAGE      20

// CORE to run on : -1 for last processor
#define CORE    -1
#define IA32_MISC_ENABLE_OFFSET 0x1a4
#define DISBALE_PREFETCH(msr)   (msr |= 0xf)

// On some systems, HW prefetch details are not well know. Use BIOS setting for
// disabling it
#define SOFTWARE_CONTROL_HWPREFETCH 1


#define MIN_BANKS       8
#define MAX_BANKS       64
#define MIN_BANK_SIZE   PAGE_SIZE

// An entry is an address we tested to see on which address it lied
#define NUM_ENTRIES    (NUM_CONTIGOUS_PAGES * PAGE_SIZE) / (MIN_BANK_SIZE)
#define MAX_NUM_ENTRIES_IN_BANK (NUM_ENTRIES / MIN_BANKS)
typedef struct entry {
   
    uint64_t virt_addr;
    uint64_t phy_addr;                              // Physical address of entry
    int bank;                                       // Bank on which this lies
    struct entry *siblings[MAX_NUM_ENTRIES_IN_BANK];// Entries that lie on same banks
    int num_sibling;
    int associated;                                 // Is this someone's sibling?
} entry_t;

entry_t entries[NUM_ENTRIES];

// DRAM bank
typedef struct banks_t {
    int entries_found;          // Did we find any entry which fit in this bank?
    entry_t *main_entry;        // Master entry that belongs to this bank
} bank_t;

bank_t banks[MAX_BANKS];

// This is the crux of program. This function is a hypothesis of the 
// physical address to dram bank mapping function.
// It takes a physical address and returns the bank it thinks it belongs to.
// The program will try to test if this function/hypothesis is correct/complete
int phy_to_bank_mapping(uint64_t phy_addr)
{
    bool bit0 = ((phy_addr >> 14) & 1);
    bool bit1 = (((phy_addr >> 15) ^ (phy_addr >> 18)) & 1);
    bool bit2 = (((phy_addr >> 16) ^ (phy_addr >> 19)) & 1);
    bool bit3 = (((phy_addr >> 17) ^ (phy_addr >> 20)) & 1);
    
    return (bit0 | (bit1 << 1) | (bit2 << 2) | (bit3 << 3));
}

static void init_banks(void)
{
    int i;
    for (i = 0; i < MAX_BANKS; i++) {
        banks[i].entries_found = 0;
        banks[i].main_entry = NULL;
    }
}

static void init_entries(uint64_t virt_start, uintptr_t phy_start)
{
    uintptr_t inter_bank_spacing = MIN_BANK_SIZE;
    int i;
    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];
        memset(entry, 0, sizeof(*entry));
        entry->bank = -1;
        entry->phy_addr = phy_start + i * inter_bank_spacing;
        entry->virt_addr = virt_start + i * inter_bank_spacing;
        entry->num_sibling = 0;
        entry->associated = false;
    }
}

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
    dprintf("Running on core %d\n", cpu);

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
    dprintf("Old MSR:0x%lx\n", msr);

    DISBALE_PREFETCH(msr);

    ret = pwrite(fd, &msr, sizeof(msr), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't write msr dev: %s\n", strerror(errno));
        return -1;
    }

    dprintf("New MSR:0x%lx\n", msr);

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

    dprintf("New MSR:0x%lx\n", msr);

    ret = pwrite(fd, &flag, sizeof(flag), IA32_MISC_ENABLE_OFFSET);
    if (ret != sizeof(msr)) {
        fprintf(stderr, "Couldn't write msr dev\n");
        return -1;
    }

    dprintf("Set MSR:0x%lx\n", flag);

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
    uint64_t a = (uint64_t)(uintptr_t)_a;
    uint64_t b = (uint64_t)(uintptr_t)_b;
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
                          "addl (%2), %0\n\t"
                          "clflush (%1)\n\t"
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
        if ((double)(ticks) > threshold) {
            i--;
            continue;
        }
        min_ticks = ticks < min_ticks ? ticks : min_ticks;
        max_ticks = ticks > max_ticks ? ticks : max_ticks;
        sum_ticks += ticks;
    }

    avg_ticks = (sum_ticks * 1.0f) / MAX_OUTER_LOOP;
    dprintf("Avg Ticks: %0.3f,\tMax Ticks: %ld,\tMin Ticks: %ld\n",
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
       
        dprintf("Found contiguous pages\n");
        for (int i = 0; i < found; i++) {
            uintptr_t v = start + i * PAGE_SIZE;
            dprintf("Virt:0x%lx, Phy:0x%lx\n", v,  get_physical_addr(v));
        }
        return (void *)start;
    }

    return NULL;
}

#if (KERNEL_ALLOCATOR_MODULE==1)
void *mmap_contiguous(size_t len, uint64_t *phy_start_addr)
{
    void *ret;
    int iret;
    int fd = open(KERNEL_ALLOCATOR_MODULE_FILE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Couldn't open device file\n");
        return NULL;
    }

    ret = mmap(NULL, len, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, 0);

    /* We don't close the file. We let it close when exit */
    if (ret == MAP_FAILED) {
        fprintf(stderr, "Couldn't allocate memory from device\n");
        return MAP_FAILED;
    }

    // Get the physcal address of start address
    iret = ioctl(fd, 0, phy_start_addr);
    if (iret < 0) {
        fprintf(stderr, "Couldn't find the physical address of start\n");
        return MAP_FAILED;
    }

    dprintf("Device allocate: Virt Addr: %p, Phy Addr: %p, Len: 0x%lx\n",
            ret, (void *)*phy_start_addr, len);
   
    /*
     * TODO: Find why /proc/self/pagemap is not having proper entry 
     * for this page

    for (int i = 0; i < len / PAGE_SIZE; i++) {
        uintptr_t v = (uintptr_t)(ret) + i * PAGE_SIZE;
        printf("Virt:0x%lx, Phy:0x%lx\n", v,  get_physical_addr(v));
    }
    */

    return ret;
}
#endif // KERNEL_ALLOCATOR_MODULE==1

/* Tries to allocate physical contigous pages and return the start address */
void *allocate_contigous(int contiguous_pages, uintptr_t *phy_start) {
    
    int max_itr = MAX_MMAP_ITR;
    int i;
    size_t len = MEM_SIZE;

    for (i = 0; i < max_itr; i++) {
#if (KERNEL_ALLOCATOR_MODULE == 1)
        void *virt_start = mmap_contiguous(len, phy_start);
#else
        void *virt_start = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                           -1, 0);
#endif
        if(virt_start == MAP_FAILED || virt_start == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            return NULL;
        }
        assert(mlock(virt_start, len) == 0);

#if (KERNEL_ALLOCATOR_MODULE == 1)
        // Allocation by kernel is always contigous
        return virt_start;
#endif
        void *ret = is_contiguous(virt_start, len, contiguous_pages);
        if (ret != NULL) {
            *phy_start = get_physical_addr((uintptr_t)virt_start);
            return ret;
        }

        assert(munlock(virt_start, len) == 0);
        assert(munmap(virt_start, len) == 0);
    }

    return NULL;
}

void run_exp(uint64_t virt_start, uint64_t phy_start)
{
    uintptr_t a, b;
    double threshold = LONG_MAX;
    double avg, sum, running_avg, running_threshold;
    double *avgs;
    int i, j, num_outlier;

    // Warm up - Get refined threshold 
    a = virt_start;
    b = a + sizeof(uint64_t);
    avg = find_read_time((void *)a, (void *)b, threshold);
    threshold = avg * THRESHOLD_MULTIPLIER;

    dprintf("Threshold is %f\n", threshold);

    for (i = 0; i < NUM_ENTRIES; i++) {

        entry_t *entry = &entries[i];         
        int sub_entries = NUM_ENTRIES - (i + 1);
        
        if (entry->associated)
            continue;

        dprintf("Master Entry: %d\n", i);

        avgs = calloc(sizeof(double), NUM_ENTRIES);
        assert(avgs != NULL);
        
        for (j = i + 1, sum = 0; j < NUM_ENTRIES; j++) {
            a = entries[i].virt_addr;
            b = entries[j].virt_addr;
            avgs[j] = find_read_time((void *)a, (void *)b, threshold);
            sum += avgs[j];
        }

        running_avg = sum / sub_entries;
        running_threshold = (running_avg * (100.0 + OUTLIER_PERCENTAGE)) / 100.0;

        for (j = i + 1, num_outlier = 0; j < NUM_ENTRIES; j++) {
            if (avgs[j] >= running_threshold) {
                if (entries[j].associated) {
                    fprintf(stderr, "Entry being mapped to multiple siblings\n");
                    fprintf(stderr, "Entry: PhyAddr: 0x%lx,"
                            " Prior Sibling: PhyAddr: 0x%lx,"
                            " Current Sibling: PhyAddr: 0x%lx\n",
                            entries[j].phy_addr, entries[j].siblings[0]->phy_addr,
                            entry->phy_addr);
                } else {
                    entry->siblings[num_outlier] = &entries[j];
                    num_outlier++;
                    entries[j].associated = true;
                    entries[j].siblings[0] = entry;
                    entry->num_sibling = 1;
                }   
            }
        }
        entry->num_sibling = num_outlier;
        entry->associated = false;
    }
}

// Checks mapping/hypothesis
// TODO: Check if all the bits of address have been accounted for
void check_mapping(void)
{
    int i, j;
    int main_bank, bank;
    int num_true_siblings;

    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];

        // Look for only master siblings
        if (entry->associated == true)
            continue;

        main_bank = phy_to_bank_mapping(entry->phy_addr);
        entry->bank = main_bank;
        for (j = 0, num_true_siblings = 0; j < entry->num_sibling; j++) {
            entry_t *sibling = entry->siblings[j];
            bank = phy_to_bank_mapping(sibling->phy_addr);
            sibling->bank = bank;
            if (bank != main_bank) {
                fprintf(stderr, "Banks not match for siblings\n");
                fprintf(stderr, "Main: PhyAddr: 0x%lx Bank:%d, "
                                "Sibling: PhyAddr: 0x%lx Bank: %d\n",
                                entry->phy_addr, main_bank,
                                sibling->phy_addr, bank);
            } else {
                num_true_siblings++;
            }
        }

        if (banks[main_bank].entries_found != 0) {
            fprintf(stderr, "Multiple entries belong to same bank\n");
            fprintf(stderr, "Hypothesis might be insufficient\n");
            fprintf(stderr, "Bank: %d, Earlier Main Entry: PhyAddr: 0x%lx, "
                            "Current Main Entry: PhyAddr: 0x:%lx\n",
                            main_bank, banks[main_bank].main_entry->phy_addr,
                            entry->phy_addr);
        } else {
            banks[main_bank].entries_found = num_true_siblings + 1;
        }
    }

    // All entries should be assigned a bank
    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];
        if (entry->bank < 0) {
            fprintf(stderr, "Entry not assigned any bank: PhyAddr: 0x%lx\n",
                    entry->phy_addr);
        }
    }

    // Print bank stats
    printf("Banks in use: Total Entries: %d\n", NUM_ENTRIES);
    for (i = 0; i < MAX_BANKS; i++) {
        if (banks[i].entries_found == 0)
            continue;
        printf("Bank:%d, Entries:%d\n", i, banks[i].entries_found);
    }
}

int main()
{
    void *virt_start;
    uint64_t phy_start;

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

    virt_start = allocate_contigous(NUM_CONTIGOUS_PAGES, &phy_start);
    if (virt_start == NULL) {
        fprintf(stderr, "Couldn't find the physical contiguous addresses\n");
        return -1;
    }

    init_banks();
    init_entries((uint64_t)virt_start, phy_start);
   
    run_exp((uint64_t)virt_start, phy_start);

    check_mapping();

#if (SOFTWARE_CONTROL_HWPREFETCH == 1)
    ret = enable_prefetch(core, pflag);
    if (ret < 0) {
        fprintf(stderr, "ERROR:Couldn't reset prefetching\n");
        return -1;
    }
#endif
    return 0;
}