#include "common/pthreads_cross.h"

unsigned int pcthread_get_num_procs() {
    return 1;
}

void ms_to_timespec(struct timespec *ts, unsigned int ms) {
    if (!ts) {
        return;
    }
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = (long)((ms % 1000) * 1000000L);
}

unsigned int timespec_to_ms(const struct timespec *abstime) {
    if (!abstime) {
        return 0;
    }
    return (unsigned int)(abstime->tv_sec * 1000U + (abstime->tv_nsec / 1000000L));
}
