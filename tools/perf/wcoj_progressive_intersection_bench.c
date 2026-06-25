#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Posting
{
    uint64_t vertex_id;
    uint32_t source_index;
} Posting;

typedef struct DirtyBitmap
{
    uint64_t *words;
    size_t *dirty_word_indexes;
    size_t word_count;
    size_t dirty_word_count;
} DirtyBitmap;

static volatile uint64_t benchmark_sink;

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
}

static void *checked_calloc(size_t count, size_t bytes)
{
    void *result;

    if (bytes != 0 && count > SIZE_MAX / bytes)
    {
        fprintf(stderr, "allocation size overflow\n");
        exit(EXIT_FAILURE);
    }
    result = calloc(count, bytes);
    if (result == NULL)
    {
        fprintf(stderr, "allocation failed for %zu bytes\n", count * bytes);
        exit(EXIT_FAILURE);
    }
    return result;
}

static int64_t parse_positive(const char *name, const char *value)
{
    char *end = NULL;
    long long parsed;

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0)
    {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return (int64_t) parsed;
}

static uint64_t mix_digest(uint64_t digest, const Posting *posting)
{
    digest ^= posting->vertex_id + UINT64_C(0x9e3779b97f4a7c15) +
              (digest << 6) + (digest >> 2);
    digest ^= (uint64_t) posting->source_index *
              UINT64_C(0xbf58476d1ce4e5b9);
    return digest;
}

static void dirty_bitmap_reset(DirtyBitmap *bitmap)
{
    size_t i;

    for (i = 0; i < bitmap->dirty_word_count; i++)
        bitmap->words[bitmap->dirty_word_indexes[i]] = 0;
    bitmap->dirty_word_count = 0;
}

static bool dirty_bitmap_add(DirtyBitmap *bitmap, uint32_t source_index)
{
    size_t word_index = source_index >> 6;
    uint64_t bit = UINT64_C(1) << (source_index & 63);
    uint64_t word = bitmap->words[word_index];

    if ((word & bit) != 0)
        return false;
    if (word == 0)
        bitmap->dirty_word_indexes[bitmap->dirty_word_count++] = word_index;
    bitmap->words[word_index] = word | bit;
    return true;
}

__attribute__((noinline))
static int64_t global_intersection_full_reset(const Posting *postings,
                                              size_t posting_count,
                                              int64_t source_count,
                                              uint64_t *words,
                                              size_t word_count)
{
    uint64_t current_vertex = 0;
    uint64_t digest = 0;
    int64_t distinct_seen = 0;
    int64_t output_count = 0;
    bool have_current = false;
    size_t i;

    memset(words, 0, word_count * sizeof(*words));
    for (i = 0; i < posting_count; i++)
    {
        const Posting *posting = &postings[i];
        size_t word_index;
        uint64_t bit;

        if (!have_current || posting->vertex_id != current_vertex)
        {
            if (have_current && distinct_seen == source_count)
                output_count++;
            memset(words, 0, word_count * sizeof(*words));
            current_vertex = posting->vertex_id;
            distinct_seen = 0;
            have_current = true;
        }

        word_index = posting->source_index >> 6;
        bit = UINT64_C(1) << (posting->source_index & 63);
        if ((words[word_index] & bit) == 0)
        {
            words[word_index] |= bit;
            distinct_seen++;
        }
        digest = mix_digest(digest, posting);
    }
    if (have_current && distinct_seen == source_count)
        output_count++;
    benchmark_sink ^= digest;
    return output_count;
}

__attribute__((noinline))
static int64_t global_intersection_sparse_reset(const Posting *postings,
                                                size_t posting_count,
                                                int64_t source_count,
                                                DirtyBitmap *bitmap)
{
    uint64_t current_vertex = 0;
    uint64_t digest = 0;
    int64_t distinct_seen = 0;
    int64_t output_count = 0;
    bool have_current = false;
    size_t i;

    memset(bitmap->words, 0, bitmap->word_count * sizeof(*bitmap->words));
    bitmap->dirty_word_count = 0;
    for (i = 0; i < posting_count; i++)
    {
        const Posting *posting = &postings[i];

        if (!have_current || posting->vertex_id != current_vertex)
        {
            if (have_current && distinct_seen == source_count)
                output_count++;
            dirty_bitmap_reset(bitmap);
            current_vertex = posting->vertex_id;
            distinct_seen = 0;
            have_current = true;
        }

        if (dirty_bitmap_add(bitmap, posting->source_index))
            distinct_seen++;
        digest = mix_digest(digest, posting);
    }
    if (have_current && distinct_seen == source_count)
        output_count++;
    benchmark_sink ^= digest;
    return output_count;
}

static size_t intersect_sorted(uint64_t *candidates, size_t candidate_count,
                               const uint64_t *source_values,
                               size_t source_value_count)
{
    size_t candidate_index = 0;
    size_t source_index = 0;
    size_t output_count = 0;

    while (candidate_index < candidate_count &&
           source_index < source_value_count)
    {
        uint64_t candidate = candidates[candidate_index];
        uint64_t source_value = source_values[source_index];

        if (candidate < source_value)
            candidate_index++;
        else if (candidate > source_value)
            source_index++;
        else
        {
            candidates[output_count++] = candidate;
            candidate_index++;
            source_index++;
        }
    }
    return output_count;
}

__attribute__((noinline))
static int64_t progressive_sparse_intersection(const uint64_t *adjacency,
                                               int64_t source_count,
                                               int64_t fanout,
                                               uint64_t *candidates)
{
    size_t candidate_count = (size_t) fanout;
    uint64_t digest = 0;
    int64_t source_index;
    size_t i;

    memcpy(candidates, adjacency, candidate_count * sizeof(*candidates));
    for (i = 0; i < candidate_count; i++)
        digest ^= candidates[i] + UINT64_C(0x94d049bb133111eb);

    for (source_index = 1;
         source_index < source_count && candidate_count > 0;
         source_index++)
    {
        const uint64_t *source_values =
            adjacency + (size_t) source_index * (size_t) fanout;

        candidate_count = intersect_sorted(candidates, candidate_count,
                                           source_values, (size_t) fanout);
        digest ^= (uint64_t) candidate_count +
                  (uint64_t) source_index * UINT64_C(0x9e3779b97f4a7c15);
    }
    benchmark_sink ^= digest;
    return (int64_t) candidate_count;
}

__attribute__((noinline))
static int64_t adaptive_dense_intersection(const uint64_t *adjacency,
                                           int64_t source_count,
                                           int64_t fanout,
                                           uint64_t *candidates,
                                           const Posting *dense_postings,
                                           size_t posting_count,
                                           DirtyBitmap *bitmap)
{
    size_t seed_count = (size_t) fanout;
    size_t candidate_count;
    size_t dense_threshold;

    memcpy(candidates, adjacency, seed_count * sizeof(*candidates));
    candidate_count = intersect_sorted(candidates, seed_count,
                                       adjacency + (size_t) fanout,
                                       (size_t) fanout);
    dense_threshold = (seed_count / 4) * 3 +
                      ((seed_count % 4) * 3) / 4;
    if (candidate_count > dense_threshold)
    {
        return global_intersection_sparse_reset(
            dense_postings, posting_count, source_count, bitmap);
    }

    return progressive_sparse_intersection(adjacency, source_count, fanout,
                                           candidates);
}

static void build_sparse_data(int64_t source_count, int64_t fanout,
                              uint64_t *adjacency, Posting *postings)
{
    int64_t source;
    int64_t ordinal;

    for (source = 0; source < source_count; source++)
    {
        for (ordinal = 0; ordinal < fanout; ordinal++)
        {
            size_t index = (size_t) source * (size_t) fanout +
                           (size_t) ordinal;
            uint64_t vertex = (uint64_t) index + 1;

            adjacency[index] = vertex;
            postings[index].vertex_id = vertex;
            postings[index].source_index = (uint32_t) source;
        }
    }
}

static void build_dense_data(int64_t source_count, int64_t fanout,
                             uint64_t *adjacency, Posting *postings)
{
    int64_t source;
    int64_t ordinal;

    for (source = 0; source < source_count; source++)
    {
        for (ordinal = 0; ordinal < fanout; ordinal++)
        {
            size_t adjacency_index =
                (size_t) source * (size_t) fanout + (size_t) ordinal;

            adjacency[adjacency_index] = (uint64_t) ordinal + 1;
        }
    }
    for (ordinal = 0; ordinal < fanout; ordinal++)
    {
        for (source = 0; source < source_count; source++)
        {
            size_t posting_index =
                (size_t) ordinal * (size_t) source_count + (size_t) source;

            postings[posting_index].vertex_id = (uint64_t) ordinal + 1;
            postings[posting_index].source_index = (uint32_t) source;
        }
    }
}

static double time_full_reset(const Posting *postings, size_t posting_count,
                              int64_t source_count, uint64_t *words,
                              size_t word_count, int64_t iterations,
                              int64_t expected_result)
{
    double start = now_seconds();
    int64_t result = -1;
    int64_t i;

    for (i = 0; i < iterations; i++)
        result = global_intersection_full_reset(
            postings, posting_count, source_count, words, word_count);
    if (result != expected_result)
    {
        fprintf(stderr, "full-reset result mismatch: %" PRId64 "\n", result);
        exit(EXIT_FAILURE);
    }
    return (now_seconds() - start) / (double) iterations;
}

static double time_sparse_reset(const Posting *postings, size_t posting_count,
                                int64_t source_count, DirtyBitmap *bitmap,
                                int64_t iterations, int64_t expected_result)
{
    double start = now_seconds();
    int64_t result = -1;
    int64_t i;

    for (i = 0; i < iterations; i++)
        result = global_intersection_sparse_reset(
            postings, posting_count, source_count, bitmap);
    if (result != expected_result)
    {
        fprintf(stderr, "sparse-reset result mismatch: %" PRId64 "\n", result);
        exit(EXIT_FAILURE);
    }
    return (now_seconds() - start) / (double) iterations;
}

static double time_progressive(const uint64_t *adjacency,
                               int64_t source_count, int64_t fanout,
                               uint64_t *candidates, int64_t iterations,
                               int64_t expected_result)
{
    double start = now_seconds();
    int64_t result = -1;
    int64_t i;

    for (i = 0; i < iterations; i++)
        result = progressive_sparse_intersection(
            adjacency, source_count, fanout, candidates);
    if (result != expected_result)
    {
        fprintf(stderr, "progressive result mismatch: %" PRId64 "\n", result);
        exit(EXIT_FAILURE);
    }
    return (now_seconds() - start) / (double) iterations;
}

static double time_adaptive_dense(const uint64_t *adjacency,
                                  int64_t source_count, int64_t fanout,
                                  uint64_t *candidates,
                                  const Posting *postings,
                                  size_t posting_count,
                                  DirtyBitmap *bitmap, int64_t iterations,
                                  int64_t expected_result)
{
    double start = now_seconds();
    int64_t result = -1;
    int64_t i;

    for (i = 0; i < iterations; i++)
    {
        result = adaptive_dense_intersection(
            adjacency, source_count, fanout, candidates,
            postings, posting_count, bitmap);
    }
    if (result != expected_result)
    {
        fprintf(stderr, "adaptive dense result mismatch: %" PRId64 "\n",
                result);
        exit(EXIT_FAILURE);
    }
    return (now_seconds() - start) / (double) iterations;
}

int main(int argc, char **argv)
{
    int64_t source_count = 8192;
    int64_t fanout = 32;
    int64_t iterations = 10;
    int64_t progressive_iterations;
    size_t posting_count;
    size_t word_count;
    uint64_t *sparse_adjacency;
    uint64_t *dense_adjacency;
    uint64_t *candidates;
    uint64_t *full_words;
    Posting *sparse_postings;
    Posting *dense_postings;
    DirtyBitmap bitmap = {0};
    double full_seconds;
    double sparse_reset_seconds;
    double progressive_seconds;
    double dense_global_seconds;
    double dense_adaptive_seconds;

    if (argc > 4)
    {
        fprintf(stderr, "usage: %s [sources [fanout [iterations]]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2)
        source_count = parse_positive("sources", argv[1]);
    if (argc >= 3)
        fanout = parse_positive("fanout", argv[2]);
    if (argc >= 4)
        iterations = parse_positive("iterations", argv[3]);
    if (source_count > UINT32_MAX)
    {
        fprintf(stderr, "sources exceed uint32 range\n");
        return EXIT_FAILURE;
    }
    if ((uint64_t) source_count > SIZE_MAX / (uint64_t) fanout)
    {
        fprintf(stderr, "posting count overflow\n");
        return EXIT_FAILURE;
    }

    posting_count = (size_t) source_count * (size_t) fanout;
    word_count = (size_t) source_count / 64 +
                 ((size_t) source_count % 64 != 0);
    sparse_adjacency = checked_calloc(posting_count, sizeof(*sparse_adjacency));
    dense_adjacency = checked_calloc(posting_count, sizeof(*dense_adjacency));
    sparse_postings = checked_calloc(posting_count, sizeof(*sparse_postings));
    dense_postings = checked_calloc(posting_count, sizeof(*dense_postings));
    candidates = checked_calloc((size_t) fanout, sizeof(*candidates));
    full_words = checked_calloc(word_count, sizeof(*full_words));
    bitmap.words = checked_calloc(word_count, sizeof(*bitmap.words));
    bitmap.dirty_word_indexes =
        checked_calloc(word_count, sizeof(*bitmap.dirty_word_indexes));
    bitmap.word_count = word_count;

    build_sparse_data(source_count, fanout, sparse_adjacency, sparse_postings);
    build_dense_data(source_count, fanout, dense_adjacency, dense_postings);

    full_seconds = time_full_reset(
        sparse_postings, posting_count, source_count, full_words, word_count,
        iterations, 0);
    sparse_reset_seconds = time_sparse_reset(
        sparse_postings, posting_count, source_count, &bitmap, iterations, 0);
    progressive_iterations = iterations * source_count / 8;
    if (progressive_iterations < iterations)
        progressive_iterations = iterations;
    progressive_seconds = time_progressive(
        sparse_adjacency, source_count, fanout, candidates,
        progressive_iterations, 0);
    dense_global_seconds = time_sparse_reset(
        dense_postings, posting_count, source_count, &bitmap, iterations,
        fanout);
    dense_adaptive_seconds = time_adaptive_dense(
        dense_adjacency, source_count, fanout, candidates,
        dense_postings, posting_count, &bitmap, iterations, fanout);

    printf("sources=%" PRId64 " fanout=%" PRId64 " postings=%zu "
           "iterations=%" PRId64 " progressive_iterations=%" PRId64 " "
           "full_reset_seconds=%.9f sparse_reset_seconds=%.9f "
           "progressive_seconds=%.9f sparse_reset_speedup=%.3f "
           "progressive_speedup=%.3f dense_global_seconds=%.9f "
           "dense_adaptive_seconds=%.9f dense_overhead=%.3f "
           "sparse_result=0 dense_result=%" PRId64 "\n",
           source_count, fanout, posting_count, iterations,
           progressive_iterations, full_seconds, sparse_reset_seconds,
           progressive_seconds, full_seconds / sparse_reset_seconds,
           full_seconds / progressive_seconds, dense_global_seconds,
           dense_adaptive_seconds,
           dense_adaptive_seconds / dense_global_seconds, fanout);

    free(bitmap.dirty_word_indexes);
    free(bitmap.words);
    free(full_words);
    free(candidates);
    free(dense_postings);
    free(sparse_postings);
    free(dense_adjacency);
    free(sparse_adjacency);
    return benchmark_sink == UINT64_MAX ? EXIT_FAILURE : EXIT_SUCCESS;
}
