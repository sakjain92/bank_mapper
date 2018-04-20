#ifndef __COMMON_H__
#define __COMMON_H__

#include <inttypes.h>

// Either CPU or GPU must be the target for evaluation
#if (EVAL_CPU != 1) && (EVAL_GPU != 1)
#error "Neither CPU or GPU has been set as the target"
#endif

#if (EVAL_CPU == 1) && (EVAL_GPU == 1)
#error "Both CPU and GPU has been set as target. Only one target at a time."
#endif


#define DEBUG                           1
#if (DEBUG == 1)
#define dprintf(...)                    printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

#define eprint(...)	                    fprintf(stderr, "ERROR:" __VA_ARGS__)

#if (EVAL_CPU == 1)
#define PAGE_SHIFT                      12
#else
#define PAGE_SHIFT                      21
#endif

#define PAGE_SIZE                       (1 << PAGE_SHIFT)
#define PAGE_MASK                       (PAGE_SIZE - 1)


// Enable at most one of these option
// Priority order is: Kernel Allocator module > Huge Page > Simple Iterative mmap()
// Are we using kernel allocator module to allocate contiguous memory?
#if (EVAL_CPU==1)
#define KERNEL_ALLOCATOR_MODULE         0
#define KERNEL_ALLOCATOR_MODULE_FILE    "/dev/kam"
#define KERNEL_HUGEPAGE_ENABLED         1
#define KERNEL_HUGEPAGE_SIZE            (1024 * 1024 * 1024)    // 1 GB
#endif

#define MEM_SIZE                        (1 << 24)

// Using mmap(), we might/might not get contigous pages. We need to try multiple
// times.
// Using kernel module, if we can get, we wll get all contiguous on first attempt
#if (KERNEL_ALLOCATOR_MODULE == 1)
#define NUM_CONTIGOUS_PAGES             (MEM_SIZE / PAGE_SIZE)
#define MAX_MMAP_ITR                    1
#elif (KERNEL_HUGEPAGE_ENABLED == 1)
#define NUM_CONTIGOUS_PAGES             (MEM_SIZE > KERNEL_HUGEPAGE_SIZE ?      \
                                        (KERNEL_HUGEPAGE_SIZE / PAGE_SIZE) :    \
                                        (MEM_SIZE / PAGE_SIZE))
#define MAX_MMAP_ITR                    1
#else /* GPU */
#define NUM_CONTIGOUS_PAGES             (MEM_SIZE / PAGE_SIZE)
#define MAX_MMAP_ITR                    0
#endif

#if (EVAL_CPU==1)
#define MAX_INNER_LOOP                  10
#define MAX_OUTER_LOOP                  1000
#else
#define MAX_INNER_LOOP                  1
#define MAX_OUTER_LOOP                  10
#endif

// Threshold for timing
#define THRESHOLD_MULTIPLIER            5

// By what percentage does a timing needs to be away from average to be considered
// outlier and hence we can assume that pair of address lie on same bank, different
// rows
#ifdef EVAL_CPU
#define OUTLIER_PERCENTAGE              20
#else
#define OUTLIER_PERCENTAGE              8                                     
#endif

// CORE to run on : -1 for last processor
#define CPU_CORE                        -1
#define IA32_MISC_ENABLE_OFFSET         0x1a4
#define DISBALE_PREFETCH(msr)           (msr |= 0xf)

// On some systems, HW prefetch details are not well know. Use BIOS setting for
// disabling it
// This is only for CPU side hardware prefetching
#define SOFTWARE_CONTROL_HWPREFETCH     1

// Following values need not be exact, just approximation. Limits used for
// memory allocation
#define MIN_BANKS                       8
#define MAX_BANKS                       64
#define MIN_BANK_SIZE                   (1 << 12)

#define NUM_ENTRIES    ((NUM_CONTIGOUS_PAGES * PAGE_SIZE) / (MIN_BANK_SIZE))
#define MAX_NUM_ENTRIES_IN_BANK         (NUM_ENTRIES)
typedef struct entry {
   
    uint64_t virt_addr;
    uint64_t phy_addr;                              // Physical address of entry
    int bank;                                       // Bank on which this lies
    struct entry *siblings[MAX_NUM_ENTRIES_IN_BANK];// Entries that lie on same banks
    int num_sibling;
    int associated;                                 // Is this someone's sibling?
} entry_t;


// DRAM bank
typedef struct banks_t {
    entry_t *main_entry;        // Master entry that belongs to this bank
} bank_t;


#endif // __COMMON_H__
