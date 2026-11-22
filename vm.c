#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGICAL_ADDRESS_MAX 65535L

typedef enum {
    POLICY_FIFO,
    POLICY_LRU
} Policy;

typedef struct {
    uint16_t *items;
    size_t count;
} AddressList;

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s <addresses.txt> <fifo|lru> <fifo|lru>\n", program);
}

static int parse_policy(const char *text, Policy *policy)
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

static int append_address(AddressList *list, size_t *capacity, uint16_t address)
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

static int parse_address_line(char *line, uint16_t *address)
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

static int load_addresses(const char *path, AddressList *list)
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
        if (parse_address_line(line, &address) != 0) {
            fprintf(stderr, "Error: invalid logical address on line %zu\n", line_number);
            status = -1;
            break;
        }
        if (append_address(list, &capacity, address) != 0) {
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

int main(int argc, char **argv)
{
    if (argc != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    Policy page_policy;
    Policy tlb_policy;
    if (parse_policy(argv[2], &page_policy) != 0 ||
        parse_policy(argv[3], &tlb_policy) != 0) {
        fprintf(stderr, "Error: replacement policies must be 'fifo' or 'lru'\n");
        return EXIT_FAILURE;
    }

    AddressList addresses = {0};
    if (load_addresses(argv[1], &addresses) != 0) {
        return EXIT_FAILURE;
    }

    printf("Validated %zu logical addresses (page=%s, tlb=%s)\n",
           addresses.count,
           page_policy == POLICY_FIFO ? "fifo" : "lru",
           tlb_policy == POLICY_FIFO ? "fifo" : "lru");
    free(addresses.items);
    return EXIT_SUCCESS;
}
