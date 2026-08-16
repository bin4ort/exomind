/* exocrawl: worker pool and per-host pacing. */
#include "exocrawl.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void *pool_worker(void *arg)
{
    pool_t *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->quit)
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->quit && !p->head) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        job_t *j = p->head;
        p->head = j->next;
        if (!p->head)
            p->tail = NULL;
        p->active++;
        pthread_mutex_unlock(&p->mu);
        j->fn(j);
        pthread_mutex_lock(&p->mu);
        p->active--;
        if (p->active == 0 && !p->head)
            pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

int pool_init(pool_t *p, size_t n)
{
    memset(p, 0, sizeof *p);
    p->nthreads = n ? n : 8;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->threads = calloc(p->nthreads, sizeof(pthread_t));
    if (!p->threads)
        return -1;
    for (size_t i = 0; i < p->nthreads; i++)
        pthread_create(&p->threads[i], NULL, pool_worker, p);
    return 0;
}

void pool_submit(pool_t *p, job_fn fn, void *arg)
{
    job_t *j = calloc(1, sizeof *j);
    if (!j)
        return;
    j->fn = fn;
    j->arg = arg;
    pthread_mutex_lock(&p->mu);
    if (p->tail)
        p->tail->next = j;
    else
        p->head = j;
    p->tail = j;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

void pool_wait(pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    while (p->head || p->active)
        pthread_cond_wait(&p->cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

void pool_destroy(pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (size_t i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    free(p->threads);
    job_t *j = p->head;
    while (j) {
        job_t *nx = j->next;
        free(j);
        j = nx;
    }
}

/* ---- per-host pacing ---- */
static pthread_mutex_t g_pace_mu = PTHREAD_MUTEX_INITIALIZER;
static pace_t *g_paces;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pace_lock(void)
{
    pthread_mutex_lock(&g_pace_mu);
}

void pace_unlock(void)
{
    pthread_mutex_unlock(&g_pace_mu);
}

/* sleep until `min_ms` since the last request to host; returns ms waited */
int64_t pace_wait(const char *host, int min_ms)
{
    pace_lock();
    pace_t *p = g_paces;
    while (p && strcmp(p->host, host) != 0)
        p = p->next;
    if (!p) {
        p = calloc(1, sizeof *p);
        snprintf(p->host, sizeof p->host, "%s", host);
        p->next = g_paces;
        g_paces = p;
    }
    int64_t now = now_ms();
    int64_t wait = p->next_ms - now;
    if (wait < 0)
        wait = 0;
    p->next_ms = now + wait + min_ms;
    pace_unlock();
    if (wait > 0) {
        struct timespec ns = {(time_t)(wait / 1000),
                             (long)(wait % 1000) * 1000000L};
        (void)nanosleep(&ns, NULL);
    }
    return wait;
}
