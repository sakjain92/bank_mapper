#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DATA_FILE               "data.txt"
#define BANK_STRING             "Bank"
#define START_INDEX             11
#define END_INDEX               24
#define MAX_DEPTH               (END_INDEX - START_INDEX + 1)
#define MAX_ADDR_PER_BANK       1000
#define MAX_BANK                64
#define MAX_SOLUTION            1000

#define DEBUG                   0

typedef enum {
    OR,
    AND,
    XOR,
} ops_t;

typedef struct solution {

    int indexes[MAX_DEPTH];
    int ops[MAX_DEPTH];
    int depth;
    int valid;

} solution_t;

typedef struct solution_array {

    solution_t s[MAX_SOLUTION];
    int max_solutions;
    int num_solutions;

} solution_array_t;

solution_array_t cpu_solution_array = {

    .num_solutions = 5,
    .max_solutions = MAX_SOLUTION,
    .s = {
            {
                .valid = 1,
                .depth = 1,
                .indexes = {14}
            },
            {
                .valid = 1,
                .depth = 2,
                .ops = {XOR},
                .indexes = {15, 18}
            },
            {
                .valid = 1,
                .depth = 2,
                .ops = {XOR},
                .indexes = {16, 19}        
            },
            {
                .valid = 1,
                .depth = 2,
                .ops = {XOR},
                .indexes = {17, 20}                
            },
            {
                .valid = 1,
                .depth = 4,
                .ops = {XOR, XOR, XOR, XOR},
                .indexes = {12, 13, 15, 16}  
            }
    }   
};

void print_solution(const solution_t *s)
{
    int i;

    printf("Indexes: ");
    for (i = 0; i < s->depth; i++) {
        printf("%d ", s->indexes[i]);
    }
    printf("\n");

    printf("Ops: ");
    for (i = 0; i < s->depth - 1; i++) {
        printf("%d ", s->ops[i]);
    }
    printf("\n");
}

int check(uint64_t *addr, size_t count, const solution_t *s)
{
    size_t i;
    int j;
    int res = -1;

    assert(s->depth >= 1);

    for (i = 0; i < count; i++) {
        int curres = (addr[i] >> s->indexes[0]) & 1;
        for (j = 0; j < s->depth - 1; j++) {
            switch(s->ops[j]) {
                case OR:
                    curres = curres | (addr[i] >> s->indexes[j + 1]);
                    break;
                case AND:
                    curres = curres & (addr[i] >> s->indexes[j + 1]);
                    break;
                case XOR:
                    curres = curres ^ (addr[i] >> s->indexes[j + 1]);
                    break;
                default:
                    assert(0);
            }
            curres = curres & 0x1;
        }

        if (res != -1) {
            if (res != curres)
                return 0;
        } else {
            res = curres;
        }
    }

#if (DEBUG == 1)
    printf("Found:\n");
    print_solution(s);
#endif

    return 1;
}

int permute(int *array, int size, int min_val, int max_val, int isfirst)
{
    int i, start_index;

    assert(size >= 1);

    if (isfirst) {
        for (i = 0; i < size; i++) {
            array[i] = min_val + i;
        }

        assert(array[size - 1] <= max_val);
        return 1;
    }


    if (array[size - 1] < max_val) {
        array[size - 1]++;
        return 1;
    }

    if (size == 1)
        return 0;

    for (i = size - 2; i >= 0; i--) {
         
        array[i]++;
        if (array[i] != max_val) {
            break;
        }
    }

    start_index = i;
    
    while (start_index >= 0) {
        int valid = 1;
        
        for (i = start_index + 1; i < size; i++) {
            array[i] = array[i - 1] + 1;
            if (array[i] > max_val) {
                valid = 0;
                break;
            }
        }
        
        if (valid)
            break;

        if (start_index == 0)
            return 0;

        while (start_index > 0 && ++array[--start_index] != max_val);
    }

    if (start_index < 0)
        return 0;

    return 1;
}

void find_algo(uint64_t *addr, size_t count, solution_array_t *sarray)
{
    int i, j;
    int solutions_found = 0;
    solution_t *s;

    /* Currently only considering XORs */
    for (i = 0; i < sarray->max_solutions; i++) {
        s = &sarray->s[i];
        for (j = 0; j < MAX_DEPTH - 1; j++) {
            s->ops[j] = XOR;
        }
    }

    for (i = 0; i < MAX_DEPTH; i++) {
    
        int isFirst = 1;

        while (1) {
            s = &sarray->s[solutions_found];
            s->depth = i + 1;

            if (permute(s->indexes, s->depth, START_INDEX, END_INDEX, isFirst) == 0)
                break;
            
            if (check(addr, count, s) == 1) {
                memcpy(&sarray->s[solutions_found + 1], &sarray->s[solutions_found], sizeof(solution_t));
                s->valid = 1;
                solutions_found++;
                assert(solutions_found <= sarray->max_solutions);
            }
            
            isFirst = 0;
        }
    }

    sarray->num_solutions = solutions_found;
}

int find_intersection(solution_array_t *sarray, const solution_array_t *new_sarray)
{
    int i, j;
    int valid;
    if (sarray->num_solutions < 0) {
        memcpy(sarray, new_sarray, sizeof(solution_array_t));
        return 1;
    }

    for (i = 0; i < sarray->num_solutions; i++) {
        solution_t *s = &sarray->s[i];
        if (s->valid == 0)
            continue;
        assert(s->valid == 1);
        s->valid = -1;
    }

    for (i = 0; i < sarray->num_solutions; i++) {
        
        solution_t *s = &sarray->s[i];
        if (s->valid == 0)
            continue;
        
        assert(s->valid == -1);

        for (j = 0; j < new_sarray->num_solutions; j++) {
            const solution_t *ns = &new_sarray->s[j];
            s->valid = 1;
            if (memcmp(s, ns, sizeof(solution_t)) == 0) {
                break;
            }
            s->valid = -1;
        }
    }

    for (i = 0, valid = 0; i < sarray->num_solutions; i++) {
        solution_t *s = &sarray->s[i];

        if (s->valid == -1)
            s->valid = 0;

        assert(sarray->s[i].valid == 1 || sarray->s[i].valid == 0);
        valid += !!(sarray->s[i].valid);
    }

    if (valid)
        return 1;
    else
        return 0;
}

/* 
 * This is currently assuming only XORs ops 
 * Also assumes depth is increases in solution array 
 */
void find_unique(solution_array_t *sarray)
{
    int i, j, k, l;
    int found_index;
    for (i = 0; i < sarray->num_solutions; i++) {
        solution_t *s = &sarray->s[i];
        if (s->valid == 0)
            continue;

        for (j = i + 1; j < sarray->num_solutions; j++) {
            solution_t *os = &sarray->s[j];
            if (os->valid == 0)
                continue;

            found_index = 0;
            for (k = 0; k < s->depth; k++) {
                for (l = 0; l < os->depth; l++) {
                    if (s->indexes[k] == os->indexes[l]) {
                        found_index++;
                        break;
                    }
                }  
            }

            if (found_index == s->depth)
                os->valid = 0;
        }
    }
}

int main()
{
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    uint64_t addr[MAX_ADDR_PER_BANK];
    size_t addr_count = 0;
    char *bank_str = BANK_STRING;
    size_t bank_str_len = strlen(bank_str);
    solution_array_t sarray;
    solution_array_t temp_sarray;
    int i;
    int count;

    sarray.max_solutions = MAX_SOLUTION;
    sarray.num_solutions = -1;

    fp = fopen(DATA_FILE, "r");
    if (fp == NULL) {
        fprintf(stderr, "Couldn't open file\n");
        exit(EXIT_FAILURE);
    }

    while (((read = getline(&line, &len, fp)) != -1) || addr_count != 0) {

        /* Found new bank */
        if (read < 0 || (strncmp(line, bank_str, bank_str_len) == 0)) {
         
            if (addr_count != 0) {
                
                temp_sarray.max_solutions = MAX_SOLUTION;
                temp_sarray.num_solutions = -1;

                find_algo(addr, addr_count, &temp_sarray);
                if (temp_sarray.num_solutions == 0)
                    goto exit;
            
                if (find_intersection(&sarray, &temp_sarray) != 1)
                    goto exit;

                /* Check for manual solution also */
                for (i = 0; i < cpu_solution_array.num_solutions; i++) {
                    if (check(addr, addr_count, &cpu_solution_array.s[i]) != 1) {
                        fprintf(stderr, "Manual solution invalid: %d\n", i);
                    }
                }   
            }

            addr_count = 0;

        } else {
            
            if (sscanf(line, "0x%lx", &addr[addr_count]) != 1) {
                printf("Line:%s\n", line);
                fprintf(stderr, "Couldn't read line\n");
                exit(EXIT_FAILURE);
            }

#if (DEBUG == 1)
            printf("Got: 0x%lx\n", addr[addr_count]);
#endif
            addr_count++;
            assert(addr_count <= MAX_ADDR_PER_BANK);
        }
    }

    find_unique(&sarray);

exit:
    fclose(fp);
    if (line)
        free(line);

    for (i = 0, count = 0; i < sarray.num_solutions; i++) {
        if (sarray.s[i].valid == 1) {
            print_solution(&sarray.s[i]);
            count++;
        }   
    }
    printf("Number of solutions:%d\n", count);

    exit(EXIT_SUCCESS);
}
