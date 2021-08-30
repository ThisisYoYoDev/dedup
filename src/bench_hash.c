#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "./hash.h"

#define STB_DS_IMPLEMENTATION
#include "./stb_ds.h"

void usage(FILE *stream)
{
    fprintf(stream, "Usage: bench_hash <SHA256SUM>\n");
}

typedef void (*Hof_Func)(const char *file_path, BYTE *buffer, size_t buffer_cap, Hash *hash);

#define BUFFER_MAX_CAP (1024*1024*1024)

#define ARRAY_LEN(xs) (sizeof(xs) / sizeof((xs)[0]))

typedef struct {
    const char *label;
    size_t value;
} Buffer_Cap_Attrib;

size_t buffer_caps[] = {
    1024,
    256*1024,
    512*1024,
    1024*1024,
    256*1024*1024,
    512*1024*1024,
};

typedef struct {
    const char *label;
    Hof_Func hof_func;
} Hof_Func_Attrib;

Hof_Func_Attrib hof_func_attribs[] = {
    {.label = "hash_of_file_libc",  .hof_func = hash_of_file_libc},
    {.label = "hash_of_file_linux", .hof_func = hash_of_file_linux},
    {.label = "hash_of_file_mmap",  .hof_func = hash_of_file_mmap},
};

#define HASH_LEN 64
#define SEP_LEN 2

typedef struct {
    const char *content_file_path;
    char *content;
    size_t content_size;
    size_t line_number;
} Hash_Sum_File;

Hash_Sum_File make_hash_sum_file(const char *content_file_path, char *content, size_t content_size)
{
    Hash_Sum_File result;
    result.content_file_path = content_file_path;
    result.content = content;
    result.content_size = content_size;
    result.line_number = 0;
    return result;
}

const char *next_hash_and_file(Hash_Sum_File *hsf, Hash *expected_hash)
{
    if (hsf->content_size > 0) {
        size_t line_size = strlen(hsf->content);

        if (line_size < HASH_LEN + SEP_LEN || !parse_hash(hsf->content, expected_hash)) {
            fprintf(stderr, "%s:%zu: ERROR: incorrect hash line\n",
                    hsf->content_file_path, hsf->line_number);
            exit(1);
        }

        const char *file_path = hsf->content + HASH_LEN + SEP_LEN;

        hsf->content += line_size + 1;
        hsf->content_size -= line_size + 1;

        return file_path;
    } else {
        return NULL;
    }
}

typedef struct {
    size_t offset;
    const char *label;
    Supported_Type type;
} Field;

// TODO: bring method to Bench_Result so you can aggregate by method
// It should be probably equal to size_t (cause our function can only aggregate by size_t-s)
typedef struct {
    size_t buffer_cap;
    size_t file_size;
    size_t elapsed_nsecs;
} Bench_Result;

// TODO: replace Bench_Data_Point with Bench_Result
// So you can have nested aggregation
typedef struct {
    size_t x;
    size_t y;
} Bench_Data_Point;

typedef struct {
    size_t key;
    Bench_Data_Point *points;
} Bench_Aggregation;

#define Nsecs_Fmt "%zu.%09zu"
#define Nsecs_Arg(arg) (arg)/1000/1000/1000, (arg)-(arg)/1000/1000/1000*1000*1000*1000

// TODO: introduce customizable y_offset
Bench_Aggregation *aggregate(Bench_Result *report, size_t key_offset, size_t x_offset)
{
    Bench_Aggregation *result = NULL;
    for (ptrdiff_t i = 0; i < arrlen(report); ++i) {
        size_t key = *(size_t*)((char*) &report[i] + key_offset);
        size_t x = *(size_t*)((char*) &report[i] + x_offset);
        Bench_Data_Point point = {
            .x = x,
            .y = report[i].elapsed_nsecs
        };

        ptrdiff_t index = hmgeti(result, key);
        if (index >= 0) {
            arrput(result[index].points, point);
        } else {
            Bench_Aggregation item = {
                .key = key,
                .points = NULL,
            };
            arrput(item.points, point);
            hmputs(result, item);
        }
    }

    return result;
}

void print_aggregation(Bench_Aggregation *agg, const char *key_label, const char *x_label, const char *y_label)
{
    for (size_t i = 0; i < hmlenu(agg); ++i) {
        printf("%s = %zu\n", key_label, agg[i].key);
        Bench_Data_Point *points = agg[i].points;

        size_t max_y = 0;
        for (size_t j = 0; j < arrlenu(points); ++j) {
            if (points[j].y > max_y) {
                max_y = points[j].y;
            }
        }

        const size_t bar_len = 30;

        printf("%s, %s\n", x_label, y_label);
        for (size_t j = 0; j < arrlenu(points); ++j) {
            double t = (double) points[j].y / (double) max_y;
            size_t bar_count = (size_t) floor(bar_len * t);
            printf("%09zu, "Nsecs_Fmt" ", points[j].x, Nsecs_Arg(points[j].y));
            for (size_t bar = 0; bar < bar_count; ++bar) {
                fputc('*', stdout);
            }
            fputc('\n', stdout);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "ERROR: no input file is provided\n");
        exit(1);
    }

    const char *content_file_path = argv[1];

    int fd = open(content_file_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: could not open file %s: %s\n",
                content_file_path, strerror(errno));
        exit(1);
    }

    struct stat statbuf;
    memset(&statbuf, 0, sizeof(statbuf));
    if (fstat(fd, &statbuf) < 0) {
        fprintf(stderr, "ERROR: could not determine the size of the file %s: %s\n",
                content_file_path, strerror(errno));
        exit(1);
    }
    size_t content_size = statbuf.st_size;

    char *content = mmap(NULL, content_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (content == MAP_FAILED) {
        fprintf(stderr, "ERROR: could not memory map file %s: %s\n",
                content_file_path, strerror(errno));
        exit(1);
    }

    if (!(content_size > 0 && content[content_size - 1] == '\0')) {
        fprintf(stderr, "ERROR: the content of file %s is not correct. Expected the output of sha256sum(1) with -z flag.\n", content_file_path);
        exit(1);
    }

    uint8_t *buffer = malloc(BUFFER_MAX_CAP);
    if (buffer == NULL) {
        fprintf(stderr, "ERROR: could not allocate memory: %s\n",
                strerror(errno));
        exit(1);
    }

    Bench_Result *report = NULL;
    for (size_t hof_func_index = 0; hof_func_index < ARRAY_LEN(hof_func_attribs); ++hof_func_index) {
        arrsetlen(report, 0);

        Hof_Func hof = hof_func_attribs[hof_func_index].hof_func;
        const char *hof_label = hof_func_attribs[hof_func_index].label;

        for (size_t buffer_cap_index = 0; buffer_cap_index < ARRAY_LEN(buffer_caps); ++buffer_cap_index) {
            size_t buffer_cap = buffer_caps[buffer_cap_index];

            Hash_Sum_File hsf = make_hash_sum_file(content_file_path, content, content_size);
            Hash expected_hash;
            const char *file_path = next_hash_and_file(&hsf, &expected_hash);
            while (file_path != NULL) {
                Hash actual_hash;
                // TODO: research how to gnuplot the results

                size_t file_size;
                {
                    struct stat statbuf;
                    if (stat(file_path, &statbuf) < 0) {
                        fprintf(stderr, "ERROR: could not determine the size of file %s: %s\n",
                                file_path, strerror(errno));
                        exit(1);
                    }
                    file_size = statbuf.st_size;
                }

                struct timespec start, end;
                if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
                    fprintf(stderr, "ERROR: could not get the current clock time: %s\n",
                            strerror(errno));
                    exit(1);
                }
                hof(file_path, buffer, buffer_cap, &actual_hash);
                if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
                    fprintf(stderr, "ERROR: could not get the current clock time: %s\n",
                            strerror(errno));
                    exit(1);
                }

                Bench_Result bench_result;
                bench_result.buffer_cap = buffer_cap;
                bench_result.file_size = file_size;
                bench_result.elapsed_nsecs = (end.tv_sec - start.tv_sec)*1000*1000*1000 + end.tv_nsec - start.tv_nsec;
                arrput(report, bench_result);

                if (memcmp(&expected_hash, &actual_hash, sizeof(Hash)) != 0) {
                    fprintf(stderr, "ERROR: unexpected hash of file %s\n", file_path);
                    char hash_cstr[32*2 + 1];
                    hash_as_cstr(expected_hash, hash_cstr);
                    fprintf(stderr, "Expected: %s\n", hash_cstr);
                    hash_as_cstr(actual_hash, hash_cstr);
                    fprintf(stderr, "Actual:   %s\n", hash_cstr);
                    exit(1);
                }

                file_path = next_hash_and_file(&hsf, &expected_hash);
            }
        }

        printf("==============================\n");
        printf("%s\n", hof_label);
        printf("==============================\n");
        Bench_Aggregation *aggregation_by_file_size =
            aggregate(report,
                      offsetof(Bench_Result, file_size),
                      offsetof(Bench_Result, buffer_cap));
        print_aggregation(aggregation_by_file_size,
                          "file_size", "buffer_cap", "elapsed_nsecs");

        // Bench_Aggregation *aggregation_by_file_size = NULL;
    }

    return 0;
}
