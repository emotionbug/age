#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum WorkKind
{
    WORK_VISIT = 0,
    WORK_BACKTRACK
} WorkKind;

typedef struct WorkItem
{
    int64_t frame_index;
    int64_t path_length;
    int64_t work_ordinal;
    int64_t arena_segment_index;
    WorkKind kind;
} WorkItem;

typedef struct OldQueue
{
    WorkItem *items;
    int64_t size;
    int64_t capacity;
} OldQueue;

typedef struct NewQueue
{
    WorkItem *items;
    int64_t head;
    int64_t size;
    int64_t capacity;
} NewQueue;

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

static void *checked_realloc(void *ptr, size_t bytes)
{
    void *result = realloc(ptr, bytes);

    if (result == NULL)
    {
        fprintf(stderr, "allocation failed for %zu bytes\n", bytes);
        exit(EXIT_FAILURE);
    }
    return result;
}

static void old_ensure(OldQueue *queue, int64_t required)
{
    if (required <= queue->capacity)
        return;
    while (queue->capacity < required)
        queue->capacity *= 2;
    queue->items = checked_realloc(queue->items,
                                   sizeof(*queue->items) *
                                   (size_t) queue->capacity);
}

static void new_ensure(NewQueue *queue, int64_t required)
{
    int64_t new_capacity;

    if (queue->head + required <= queue->capacity)
        return;

    new_capacity = queue->capacity;
    if (queue->head > 0 && required > queue->capacity / 2)
        new_capacity *= 2;
    while (new_capacity < required)
        new_capacity *= 2;

    if (new_capacity != queue->capacity)
    {
        queue->items = checked_realloc(queue->items,
                                       sizeof(*queue->items) *
                                       (size_t) new_capacity);
        queue->capacity = new_capacity;
    }

    if (queue->head > 0)
    {
        if (queue->size > 0)
            memmove(queue->items, &queue->items[queue->head],
                    sizeof(*queue->items) * (size_t) queue->size);
        queue->head = 0;
    }
}

static void old_push(OldQueue *queue, WorkItem item)
{
    old_ensure(queue, queue->size + 1);
    queue->items[queue->size++] = item;
}

static void new_push(NewQueue *queue, WorkItem item)
{
    new_ensure(queue, queue->size + 1);
    queue->items[queue->head + queue->size++] = item;
}

__attribute__((noinline))
static bool old_pop_level_batch(OldQueue *queue, WorkItem *result)
{
    int64_t selected_index = -1;
    int64_t selected_path_length = 0;
    int64_t i;

    if (queue->size == 0)
        return false;

    for (i = 0; i < queue->size; i++)
    {
        WorkItem *item = &queue->items[i];

        if (item->kind != WORK_VISIT)
            continue;
        if (selected_index >= 0 &&
            item->path_length >= selected_path_length)
            continue;
        selected_index = i;
        selected_path_length = item->path_length;
    }
    if (selected_index < 0)
        selected_index = queue->size - 1;

    *result = queue->items[selected_index];
    if (selected_index + 1 < queue->size)
    {
        memmove(&queue->items[selected_index],
                &queue->items[selected_index + 1],
                sizeof(*queue->items) *
                (size_t) (queue->size - selected_index - 1));
    }
    queue->size--;
    return true;
}

__attribute__((noinline))
static bool new_pop_level_batch(NewQueue *queue, WorkItem *result)
{
    int64_t index;

    if (queue->size == 0)
        return false;

    index = queue->head;
    *result = queue->items[index];
    if (result->kind != WORK_VISIT)
    {
        fprintf(stderr, "unexpected non-visit at queue head\n");
        exit(EXIT_FAILURE);
    }
    queue->head++;
    queue->size--;
    if (queue->size == 0)
        queue->head = 0;
    return true;
}

static bool items_equal(const WorkItem *left, const WorkItem *right)
{
    return left->frame_index == right->frame_index &&
           left->path_length == right->path_length &&
           left->work_ordinal == right->work_ordinal &&
           left->arena_segment_index == right->arena_segment_index &&
           left->kind == right->kind;
}

static WorkItem make_item(int64_t ordinal, int64_t depth)
{
    WorkItem item;

    item.frame_index = ordinal;
    item.path_length = depth;
    item.work_ordinal = ordinal;
    item.arena_segment_index = ordinal / 64;
    item.kind = WORK_VISIT;
    return item;
}

static uint64_t consume_old_frontier(int64_t width, double *seconds)
{
    OldQueue queue = {0};
    WorkItem item;
    uint64_t checksum = 0;
    int64_t i;
    double start;

    queue.capacity = 64;
    queue.items = checked_realloc(NULL, sizeof(*queue.items) *
                                        (size_t) queue.capacity);
    for (i = 0; i < width; i++)
        old_push(&queue, make_item(i, 1));

    start = now_seconds();
    while (old_pop_level_batch(&queue, &item))
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
    *seconds = now_seconds() - start;
    free(queue.items);
    benchmark_sink ^= checksum;
    return checksum;
}

static uint64_t consume_new_frontier(int64_t width, double *seconds)
{
    NewQueue queue = {0};
    WorkItem item;
    uint64_t checksum = 0;
    int64_t i;
    double start;

    queue.capacity = 64;
    queue.items = checked_realloc(NULL, sizeof(*queue.items) *
                                        (size_t) queue.capacity);
    for (i = 0; i < width; i++)
        new_push(&queue, make_item(i, 1));

    start = now_seconds();
    while (new_pop_level_batch(&queue, &item))
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
    *seconds = now_seconds() - start;
    free(queue.items);
    benchmark_sink ^= checksum;
    return checksum;
}

static uint64_t consume_old_sustained_frontier(int64_t width,
                                               double *seconds)
{
    OldQueue queue = {0};
    WorkItem item;
    uint64_t checksum = 0;
    int64_t i;
    double start;

    queue.capacity = 64;
    queue.items = checked_realloc(NULL, sizeof(*queue.items) *
                                        (size_t) queue.capacity);
    for (i = 0; i < width; i++)
        old_push(&queue, make_item(i, 1));

    start = now_seconds();
    for (i = 0; i < width; i++)
    {
        if (!old_pop_level_batch(&queue, &item))
        {
            fprintf(stderr, "old sustained queue drained early\n");
            exit(EXIT_FAILURE);
        }
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
        old_push(&queue, make_item(width + i, 2));
    }
    while (old_pop_level_batch(&queue, &item))
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
    *seconds = now_seconds() - start;
    free(queue.items);
    benchmark_sink ^= checksum;
    return checksum;
}

static uint64_t consume_new_sustained_frontier(int64_t width,
                                               double *seconds)
{
    NewQueue queue = {0};
    WorkItem item;
    uint64_t checksum = 0;
    int64_t i;
    double start;

    queue.capacity = 64;
    queue.items = checked_realloc(NULL, sizeof(*queue.items) *
                                        (size_t) queue.capacity);
    for (i = 0; i < width; i++)
        new_push(&queue, make_item(i, 1));

    start = now_seconds();
    for (i = 0; i < width; i++)
    {
        if (!new_pop_level_batch(&queue, &item))
        {
            fprintf(stderr, "new sustained queue drained early\n");
            exit(EXIT_FAILURE);
        }
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
        new_push(&queue, make_item(width + i, 2));
    }
    while (new_pop_level_batch(&queue, &item))
        checksum = checksum * UINT64_C(11400714819323198485) +
                   (uint64_t) item.work_ordinal + 1;
    *seconds = now_seconds() - start;
    free(queue.items);
    benchmark_sink ^= checksum;
    return checksum;
}

static void verify_interleaved_fifo(void)
{
    OldQueue old_queue = {0};
    NewQueue new_queue = {0};
    WorkItem old_item;
    WorkItem new_item;
    int64_t next_ordinal = 0;
    int64_t i;

    old_queue.capacity = 64;
    new_queue.capacity = 64;
    old_queue.items = checked_realloc(NULL, sizeof(*old_queue.items) * 64);
    new_queue.items = checked_realloc(NULL, sizeof(*new_queue.items) * 64);

    /* Wide first level, then append deeper work while the prefix is consumed. */
    for (i = 0; i < 96; i++)
    {
        WorkItem item = make_item(next_ordinal++, 1);
        old_push(&old_queue, item);
        new_push(&new_queue, item);
    }

    for (i = 0; i < 96; i++)
    {
        if (!old_pop_level_batch(&old_queue, &old_item) ||
            !new_pop_level_batch(&new_queue, &new_item) ||
            !items_equal(&old_item, &new_item))
        {
            fprintf(stderr, "level-1 order mismatch at item %" PRId64 "\n", i);
            exit(EXIT_FAILURE);
        }

        /* Two children per visited item force several prefix compactions. */
        WorkItem child1 = make_item(next_ordinal++, 2);
        WorkItem child2 = make_item(next_ordinal++, 2);
        old_push(&old_queue, child1);
        old_push(&old_queue, child2);
        new_push(&new_queue, child1);
        new_push(&new_queue, child2);
    }

    i = 0;
    while (old_pop_level_batch(&old_queue, &old_item))
    {
        if (!new_pop_level_batch(&new_queue, &new_item) ||
            !items_equal(&old_item, &new_item))
        {
            fprintf(stderr, "level-2 order mismatch at item %" PRId64 "\n", i);
            exit(EXIT_FAILURE);
        }
        i++;
    }
    if (new_pop_level_batch(&new_queue, &new_item))
    {
        fprintf(stderr, "new queue retained extra work\n");
        exit(EXIT_FAILURE);
    }
    if (i != 192)
    {
        fprintf(stderr, "unexpected level-2 count: %" PRId64 "\n", i);
        exit(EXIT_FAILURE);
    }

    free(old_queue.items);
    free(new_queue.items);
}

static int64_t parse_width(const char *value)
{
    char *end = NULL;
    long long parsed;

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0)
    {
        fprintf(stderr, "invalid width: %s\n", value);
        exit(EXIT_FAILURE);
    }
    return (int64_t) parsed;
}

int main(int argc, char **argv)
{
    int64_t width = 32768;
    double drain_old_seconds;
    double drain_new_seconds;
    double sustained_old_seconds;
    double sustained_new_seconds;
    uint64_t drain_old_checksum;
    uint64_t drain_new_checksum;
    uint64_t sustained_old_checksum;
    uint64_t sustained_new_checksum;

    if (argc > 2)
    {
        fprintf(stderr, "usage: %s [frontier-width]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2)
        width = parse_width(argv[1]);

    verify_interleaved_fifo();
    drain_old_checksum = consume_old_frontier(width, &drain_old_seconds);
    drain_new_checksum = consume_new_frontier(width, &drain_new_seconds);
    sustained_old_checksum =
        consume_old_sustained_frontier(width, &sustained_old_seconds);
    sustained_new_checksum =
        consume_new_sustained_frontier(width, &sustained_new_seconds);
    if (drain_old_checksum != drain_new_checksum ||
        sustained_old_checksum != sustained_new_checksum)
    {
        fprintf(stderr,
                "checksum mismatch: drain=%" PRIu64 "/%" PRIu64
                " sustained=%" PRIu64 "/%" PRIu64 "\n",
                drain_old_checksum, drain_new_checksum,
                sustained_old_checksum, sustained_new_checksum);
        return EXIT_FAILURE;
    }

    printf("frontier_width=%" PRId64 " item_bytes=%zu "
           "drain_old_seconds=%.9f drain_new_seconds=%.9f "
           "drain_speedup=%.3f sustained_old_seconds=%.9f "
           "sustained_new_seconds=%.9f sustained_speedup=%.3f "
           "drain_checksum=%" PRIu64 " sustained_checksum=%" PRIu64 "\n",
           width, sizeof(WorkItem), drain_old_seconds, drain_new_seconds,
           drain_old_seconds / drain_new_seconds, sustained_old_seconds,
           sustained_new_seconds,
           sustained_old_seconds / sustained_new_seconds,
           drain_old_checksum, sustained_old_checksum);
    return benchmark_sink == UINT64_MAX ? EXIT_FAILURE : EXIT_SUCCESS;
}
