#include "libusockets.h"
#include "internal/common.h"
#include <stdlib.h>

#ifdef LIBUS_USE_EPOLL

// loop
struct us_loop *us_create_loop(void (*wakeup_cb)(struct us_loop *loop), void (*pre_cb)(struct us_loop *loop), void (*post_cb)(struct us_loop *loop), int ext_size) {
    struct us_loop *loop = (struct us_loop *) malloc(sizeof(struct us_loop) + ext_size);
    loop->num_polls = 0;
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);

    us_internal_loop_data_init(loop, wakeup_cb, pre_cb, post_cb);
    return loop;
}

void us_loop_run(struct us_loop *loop) {
    us_timer_set(loop->data.sweep_timer, (void (*)(struct us_timer *)) sweep_timer_cb, LIBUS_TIMEOUT_GRANULARITY * 1000, LIBUS_TIMEOUT_GRANULARITY * 1000);

    while (loop->num_polls) {
        loop->data.pre_cb(loop);
        int num_fd_ready = epoll_wait(loop->epfd, loop->ready_events, 1024, -1);
        for (int i = 0; i < num_fd_ready; i++) {
            struct us_poll *poll = (struct us_poll *) loop->ready_events[i].data.ptr;
            us_internal_dispatch_ready_poll(poll, loop->ready_events[i].events & EPOLLERR, loop->ready_events[i].events);
        }
        loop->data.post_cb(loop);
    }
}

// poll
struct us_poll *us_create_poll(struct us_loop *loop, int fallthrough, int ext_size) {
    if (!fallthrough) {
        loop->num_polls++;
    }
    return malloc(sizeof(struct us_poll) + ext_size);
}

void us_poll_free(struct us_poll *p) {
    free(p);
}

void *us_poll_ext(struct us_poll *p) {
    return p + 1;
}

void us_poll_init(struct us_poll *p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type) {
    p->state.fd = fd;
    p->state.poll_type = poll_type;
}

void us_poll_start(struct us_poll *p, struct us_loop *loop, int events) {
    struct epoll_event event;
    event.events = events;
    event.data.ptr = p;
    epoll_ctl(loop->epfd, EPOLL_CTL_ADD, p->state.fd, &event);
}

void us_poll_change(struct us_poll *p, struct us_loop *loop, int events) {
    struct epoll_event event;
    event.events = events;
    event.data.ptr = p;
    epoll_ctl(loop->epfd, EPOLL_CTL_MOD, p->state.fd, &event);
}

LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll *p) {
    return p->state.fd;
}

int us_internal_poll_type(struct us_poll *p) {
    return p->state.poll_type;
}

void us_poll_stop(struct us_poll *p, struct us_loop *loop) {
    struct epoll_event event;
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, p->state.fd, &event);
}

unsigned int us_internal_accept_poll_event(struct us_poll *p) {
    int fd = us_poll_fd(p);
    uint64_t buf;
    read(fd, &buf, 8);
    return buf;
}

// timer
struct us_timer *us_create_timer(struct us_loop *loop, int fallthrough, int ext_size) {
    struct us_poll *p = us_create_poll(loop, fallthrough, sizeof(struct us_internal_callback) + ext_size);
    us_poll_init(p, timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC), POLL_TYPE_CALLBACK);

    struct us_internal_callback *cb = (struct us_internal_callback *) p;
    cb->loop = loop;
    cb->cb_expects_the_loop = 0;

    return (struct us_timer *) cb;
}

void us_timer_set(struct us_timer *t, void (*cb)(struct us_timer *t), int ms, int repeat_ms) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) t;

    internal_cb->cb = (void (*)(struct us_internal_callback *)) cb;

    struct itimerspec timer_spec = {
        {repeat_ms / 1000, repeat_ms % 1000000},
        {ms / 1000, ms % 1000000}
    };

    timerfd_settime(us_poll_fd((struct us_poll *) t), 0, &timer_spec, NULL);
    us_poll_start((struct us_poll *) t, internal_cb->loop, LIBUS_SOCKET_READABLE);
}

struct us_loop *us_timer_loop(struct us_timer *t) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) t;

    return internal_cb->loop;
}

// async (internal only)
struct us_internal_async *us_internal_create_async(struct us_loop *loop, int fallthrough, int ext_size) {
    struct us_poll *p = us_create_poll(loop, fallthrough, sizeof(struct us_internal_callback) + ext_size);
    us_poll_init(p, eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), POLL_TYPE_CALLBACK);

    struct us_internal_callback *cb = (struct us_internal_callback *) p;
    cb->loop = loop;
    cb->cb_expects_the_loop = 1;

    return (struct us_internal_async *) cb;
}

void us_internal_async_set(struct us_internal_async *a, void (*cb)(struct us_internal_async *)) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) a;

    internal_cb->cb = (void (*)(struct us_internal_callback *)) cb;

    us_poll_start((struct us_poll *) a, internal_cb->loop, LIBUS_SOCKET_READABLE);
}

void us_internal_async_wakeup(struct us_internal_async *a) {
    uint64_t one = 1;
    write(us_poll_fd((struct us_poll *) a), &one, 8);
}

#endif
