#include <stdlib.h>
#include "common/workerpool.h"
#include "common/zarray.h"

struct workerpool_task {
    void (*fn)(void *);
    void *data;
};

struct workerpool {
    int nthreads;
    zarray_t *tasks;
};

typedef struct workerpool workerpool_t;

workerpool_t *workerpool_create(int nthreads) {
    workerpool_t *wp = (workerpool_t *)calloc(1, sizeof(workerpool_t));
    if (!wp) {
        return NULL;
    }
    wp->nthreads = (nthreads <= 0) ? 1 : nthreads;
    wp->tasks = zarray_create(sizeof(struct workerpool_task));
    if (!wp->tasks) {
        free(wp);
        return NULL;
    }
    return wp;
}

void workerpool_destroy(workerpool_t *wp) {
    if (!wp) {
        return;
    }
    if (wp->tasks) {
        zarray_destroy(wp->tasks);
    }
    free(wp);
}

void workerpool_add_task(workerpool_t *wp, void (*f)(void *p), void *p) {
    if (!wp || !wp->tasks || !f) {
        return;
    }
    struct workerpool_task task = { .fn = f, .data = p };
    zarray_add(wp->tasks, &task);
}

static void workerpool_run_impl(workerpool_t *wp) {
    if (!wp || !wp->tasks) {
        return;
    }
    for (int i = 0; i < zarray_size(wp->tasks); i++) {
        struct workerpool_task task;
        zarray_get(wp->tasks, i, &task);
        if (task.fn) {
            task.fn(task.data);
        }
    }
    zarray_clear(wp->tasks);
}

void workerpool_run(workerpool_t *wp) {
    workerpool_run_impl(wp);
}

void workerpool_run_single(workerpool_t *wp) {
    workerpool_run_impl(wp);
}

int workerpool_get_nthreads(workerpool_t *wp) {
    if (!wp) {
        return 0;
    }
    return (wp->nthreads <= 0) ? 1 : wp->nthreads;
}

int workerpool_get_nprocs() {
    return 1;
}
