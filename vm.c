#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGICAL_ADDRESS_MAX 65535L
#define PAGE_SIZE 256
#define PAGE_COUNT 256
#define FRAME_COUNT 128
#define TLB_SIZE 16
#define BACKING_STORE_FILE "BACKING_STORE.bin"
#define OUTPUT_FILE "correct.txt"

typedef enum {
    POLICY_FIFO,
    POLICY_LRU
} Policy;

typedef struct {
    uint16_t *items;
    size_t count;
} AddressList;

typedef struct {
    uint8_t page;
    int frame;
    int valid;
    uint64_t last_used;
} TlbEntry;

typedef struct {
    int page_table[PAGE_COUNT];
    int frame_to_page[FRAME_COUNT];
    int8_t backing_store[PAGE_COUNT * PAGE_SIZE];
    int8_t physical_memory[FRAME_COUNT * PAGE_SIZE];
    size_t next_free_frame;
    size_t next_fifo_frame;
    size_t page_faults;
    TlbEntry tlb[TLB_SIZE];
    size_t next_fifo_tlb;
    size_t tlb_hits;
    uint64_t page_last_used[PAGE_COUNT];
    uint64_t clock;
} Machine;

typedef struct {
    const TlbEntry *entry;
    uint8_t page;
    int matched;
} TlbSearchTask;

static void show_usage(const char *program);
static int policy_from_text(const char *text, Policy *policy);
static int read_address_file(const char *path, AddressList *list);
static int read_backing_store(Machine *machine);
static void machine_init(Machine *machine);
static int translate_addresses(
    Machine *machine, const AddressList *addresses,
    Policy page_policy, Policy tlb_policy);

int main(int argc, char **argv)
{
    if (argc != 4) {
        show_usage(argv[0]);
        return EXIT_FAILURE;
    }

    Policy page_policy;
    Policy tlb_policy;

    if (policy_from_text(argv[2], &page_policy) != 0 ||
        policy_from_text(argv[3], &tlb_policy) != 0) {
        fprintf(stderr,
                "Error: replacement policies must be 'fifo' or 'lru'\n");
        return EXIT_FAILURE;
    }

    AddressList addresses = {0};
    if (read_address_file(argv[1], &addresses) != 0) {
        return EXIT_FAILURE;
    }

    Machine machine;
    machine_init(&machine);

    if (read_backing_store(&machine) != 0) {
        free(addresses.items);
        return EXIT_FAILURE;
    }

    int status = translate_addresses(
        &machine, &addresses, page_policy, tlb_policy);
    free(addresses.items);

    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void show_usage(const char *program)
{
    fprintf(stderr, "Usage: %s <addresses.txt> <fifo|lru> <fifo|lru>\n", program);
}

static int policy_from_text(const char *text, Policy *policy)
{
    if (strcmp(text, "fifo") == 0) {
        *policy = POLICY_FIFO;
        return 0;
    }
    if (strcmp(text, "lru") == 0) {
        *policy = POLICY_LRU;
        return 0;
    }
    return -1;
}

static int address_list_push(AddressList *list,
                             size_t *capacity,
                             uint16_t address)
{
    if (list->count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 128 : *capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(*list->items)) {
            return -1;
        }
        uint16_t *resized = realloc(list->items, new_capacity * sizeof(*list->items));
        if (resized == NULL) {
            return -1;
        }
        list->items = resized;
        *capacity = new_capacity;
    }
    list->items[list->count++] = address;
    return 0;
}

static int parse_logical_address(char *line, uint16_t *address)
{
    char *cursor = line;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(cursor, &end, 10);
    if (errno == ERANGE || end == cursor || value < 0 || value > LOGICAL_ADDRESS_MAX) {
        return -1;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }

    *address = (uint16_t)value;
    return 0;
}

static int read_address_file(const char *path, AddressList *list)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: unable to open address file '%s'\n", path);
        return -1;
    }

    char *line = NULL;
    size_t line_capacity = 0;
    size_t capacity = 0;
    size_t line_number = 0;
    int status = 0;

    while (getline(&line, &line_capacity, file) != -1) {
        line_number++;
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }

        uint16_t address = 0;
        if (parse_logical_address(line, &address) != 0) {
            fprintf(stderr, "Error: invalid logical address on line %zu\n", line_number);
            status = -1;
            break;
        }
        if (address_list_push(list, &capacity, address) != 0) {
            fprintf(stderr, "Error: unable to allocate address list\n");
            status = -1;
            break;
        }
    }

    if (ferror(file)) {
        fprintf(stderr, "Error: unable to read address file '%s'\n", path);
        status = -1;
    } else if (status == 0 && list->count == 0) {
        fprintf(stderr, "Error: address file is empty\n");
        status = -1;
    }

    free(line);
    fclose(file);
    if (status != 0) {
        free(list->items);
        list->items = NULL;
        list->count = 0;
    }
    return status;
}

static int read_backing_store(Machine *machine)
{
    FILE *file = fopen(BACKING_STORE_FILE, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: unable to open %s\n", BACKING_STORE_FILE);
        return -1;
    }

    size_t expected = sizeof(machine->backing_store);
    size_t bytes_read = fread(machine->backing_store, 1, expected, file);
    int extra = fgetc(file);
    if (bytes_read != expected || extra != EOF || ferror(file)) {
        fprintf(stderr, "Error: %s must contain exactly %zu bytes\n",
                BACKING_STORE_FILE, expected);
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static void machine_init(Machine *machine)
{
    memset(machine, 0, sizeof(*machine));
    for (size_t page = 0; page < PAGE_COUNT; page++) {
        machine->page_table[page] = -1;
    }
    for (size_t frame = 0; frame < FRAME_COUNT; frame++) {
        machine->frame_to_page[frame] = -1;
    }
}

static void tlb_invalidate_page(Machine *machine, int page)
{
    for (size_t slot = 0; slot < TLB_SIZE; slot++) {
        if (machine->tlb[slot].valid && machine->tlb[slot].page == page) {
            machine->tlb[slot].valid = 0;
        }
    }
}

static int get_or_load_frame(
    Machine *machine, uint8_t page,
    Policy page_policy, uint64_t access_time)
{
    int frame = machine->page_table[page];
    if (frame >= 0) {
        machine->page_last_used[page] = access_time;
        return frame;
    }

    if (machine->next_free_frame < FRAME_COUNT) {
        frame = (int)machine->next_free_frame++;
    } else if (page_policy == POLICY_FIFO) {
        frame = (int)machine->next_fifo_frame;
        machine->next_fifo_frame = (machine->next_fifo_frame + 1) % FRAME_COUNT;
        int evicted_page = machine->frame_to_page[frame];
        tlb_invalidate_page(machine, evicted_page);
        machine->page_table[evicted_page] = -1;
    } else {
        int evicted_page = machine->frame_to_page[0];
        for (size_t candidate = 1; candidate < FRAME_COUNT; candidate++) {
            int candidate_page = machine->frame_to_page[candidate];
            if (machine->page_last_used[candidate_page] <
                machine->page_last_used[evicted_page]) {
                evicted_page = candidate_page;
            }
        }
        frame = machine->page_table[evicted_page];
        tlb_invalidate_page(machine, evicted_page);
        machine->page_table[evicted_page] = -1;
    }

    memcpy(&machine->physical_memory[(size_t)frame * PAGE_SIZE],
           &machine->backing_store[(size_t)page * PAGE_SIZE],
           PAGE_SIZE);
    machine->page_table[page] = frame;
    machine->frame_to_page[frame] = page;
    machine->page_last_used[page] = access_time;
    machine->page_faults++;
    return frame;
}

static void *tlb_probe_worker(void *argument)
{
    TlbSearchTask *task = argument;
    task->matched = task->entry->valid && task->entry->page == task->page;
    return NULL;
}

static int tlb_lookup_parallel(const Machine *machine,
                               uint8_t page, int *frame)
{
    pthread_t threads[TLB_SIZE];
    TlbSearchTask tasks[TLB_SIZE];
    size_t created = 0;

    for (size_t slot = 0; slot < TLB_SIZE; slot++) {
        tasks[slot].entry = &machine->tlb[slot];
        tasks[slot].page = page;
        tasks[slot].matched = 0;
        int error = pthread_create(
            &threads[slot], NULL, tlb_probe_worker, &tasks[slot]);
        if (error != 0) {
            fprintf(stderr, "Error: unable to create TLB search thread: %s\n",
                    strerror(error));
            break;
        }
        created++;
    }

    for (size_t slot = 0; slot < created; slot++) {
        int error = pthread_join(threads[slot], NULL);
        if (error != 0) {
            fprintf(stderr, "Error: unable to join TLB search thread: %s\n",
                    strerror(error));
            return -2;
        }
    }
    if (created != TLB_SIZE) {
        return -2;
    }

    for (size_t slot = 0; slot < TLB_SIZE; slot++) {
        if (tasks[slot].matched) {
            *frame = machine->tlb[slot].frame;
            return (int)slot;
        }
    }
    return -1;
}

static int tlb_store(Machine *machine, uint8_t page,
                     int frame, Policy tlb_policy,
                     uint64_t access_time)
{
    size_t slot = 0;
    if (tlb_policy == POLICY_FIFO) {
        slot = machine->next_fifo_tlb;
        machine->next_fifo_tlb = (machine->next_fifo_tlb + 1) % TLB_SIZE;
    } else {
        while (slot < TLB_SIZE && machine->tlb[slot].valid) {
            slot++;
        }
        if (slot == TLB_SIZE) {
            slot = 0;
            for (size_t candidate = 1; candidate < TLB_SIZE; candidate++) {
                if (machine->tlb[candidate].last_used < machine->tlb[slot].last_used) {
                    slot = candidate;
                }
            }
        }
    }
    machine->tlb[slot].page = page;
    machine->tlb[slot].frame = frame;
    machine->tlb[slot].valid = 1;
    machine->tlb[slot].last_used = access_time;
    return (int)slot;
}

static int discard_output(FILE *output)
{
    int close_status = fclose(output);
    int remove_status = remove(OUTPUT_FILE);
    if (close_status != 0) {
        fprintf(stderr, "Error: unable to close partial %s\n", OUTPUT_FILE);
    }
    if (remove_status != 0 && errno != ENOENT) {
        fprintf(stderr, "Error: unable to remove partial %s\n", OUTPUT_FILE);
    }
    return -1;
}

static int translate_addresses(
    Machine *machine, const AddressList *addresses,
    Policy page_policy, Policy tlb_policy)
{
    FILE *output = fopen(OUTPUT_FILE, "w");
    if (output == NULL) {
        fprintf(stderr, "Error: unable to create %s\n", OUTPUT_FILE);
        return -1;
    }

    for (size_t index = 0; index < addresses->count; index++) {
        uint16_t logical = addresses->items[index];
        uint8_t page = (uint8_t)(logical >> 8);
        uint8_t offset = (uint8_t)(logical & 0xffu);
        uint64_t access_time = ++machine->clock;
        int frame = -1;
        int tlb_slot = tlb_lookup_parallel(machine, page, &frame);
        if (tlb_slot == -2) {
            return discard_output(output);
        }
        if (tlb_slot >= 0) {
            machine->tlb_hits++;
            machine->tlb[tlb_slot].last_used = access_time;
            machine->page_last_used[page] = access_time;
        } else {
            frame = get_or_load_frame(
                machine, page, page_policy, access_time);
            tlb_slot = tlb_store(
                machine, page, frame, tlb_policy, access_time);
        }
        size_t physical = (size_t)frame * PAGE_SIZE + offset;
        int value = machine->physical_memory[physical];

        if (fprintf(output,
                    "Virtual address: %u TLB: %d Physical address: %zu Value: %d\n",
                    (unsigned)logical, tlb_slot, physical, value) < 0) {
            fprintf(stderr, "Error: unable to write %s\n", OUTPUT_FILE);
            return discard_output(output);
        }
    }

    double page_fault_rate = (double)machine->page_faults / (double)addresses->count;
    double tlb_hit_rate = (double)machine->tlb_hits / (double)addresses->count;
    if (fprintf(output, "Number of Translated Addresses = %zu\n", addresses->count) < 0 ||
        fprintf(output, "Page Faults = %zu\n", machine->page_faults) < 0 ||
        fprintf(output, "Page Fault Rate = %.3f\n", page_fault_rate) < 0 ||
        fprintf(output, "TLB Hits = %zu\n", machine->tlb_hits) < 0 ||
        fprintf(output, "TLB Hit Rate = %.3f\n", tlb_hit_rate) < 0) {
        fprintf(stderr, "Error: unable to write summary to %s\n", OUTPUT_FILE);
        return discard_output(output);
    }

    if (fclose(output) != 0) {
        fprintf(stderr, "Error: unable to finish writing %s\n", OUTPUT_FILE);
        remove(OUTPUT_FILE);
        return -1;
    }
    return 0;
}

