/*
 * transport_shm.c — 载体 A：Linux 共享内存 + futex
 *
 * 这是今天就能在虚拟机上跑通的那一版。它和 Jailhouse 上的 ivshmem 做的是同一件事：
 *   1. 两个进程映射同一段物理内存
 *   2. 一方写完，通过一个"门铃"叫醒另一方
 *
 * 区别只在实现：这里内存来自 /dev/shm，门铃是 futex；
 * Jailhouse 上内存来自 ivshmem 的 BAR，门铃是写 doorbell 寄存器触发中断。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

struct shm_priv {
    int fd;
    char name[128];
};

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int futex_wait_ms(uint32_t *addr, uint32_t expect, int timeout_ms)
{
    struct timespec ts = {
        .tv_sec = timeout_ms / 1000,
        .tv_nsec = (timeout_ms % 1000) * 1000000L,
    };

    return syscall(SYS_futex, addr, FUTEX_WAIT, expect, &ts, NULL, 0);
}

static int futex_wake(uint32_t *addr)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, 1 << 30, NULL, NULL, 0);
}

static int shm_notify(struct rb_transport *t, int ch)
{
    rb_doorbell(t->shm, ch);
    return futex_wake(&t->shm->doorbell.v[ch]);
}

static int shm_wait(struct rb_transport *t, int ch, int timeout_ms)
{
    uint32_t seen = __atomic_load_n(&t->shm->doorbell.v[ch], __ATOMIC_ACQUIRE);
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        long left = deadline - now_ms();

        if (left <= 0)
            return 1;
        /* 门铃在两次检查之间被按过，直接返回 */
        if (__atomic_load_n(&t->shm->doorbell.v[ch], __ATOMIC_ACQUIRE) != seen)
            return 0;
        futex_wait_ms(&t->shm->doorbell.v[ch], seen, (int)left);
    }
}

static void shm_close(struct rb_transport *t)
{
    struct shm_priv *p = t->priv;

    if (t->shm && t->shm != MAP_FAILED)
        munmap(t->shm, RB_SHM_SIZE);
    if (p) {
        close(p->fd);
        free(p);
    }
    free(t);
}

struct rb_transport *rb_shm_open(const char *name, int create)
{
    struct rb_transport *t;
    struct shm_priv *p;
    struct rb_shared *s;
    int fd;

    fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s): %s\n", name, strerror(errno));
        return NULL;
    }
    if (create && ftruncate(fd, RB_SHM_SIZE) < 0) {
        fprintf(stderr, "ftruncate: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    s = mmap(NULL, RB_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    if (create && __atomic_load_n(&s->ctrl.magic, __ATOMIC_ACQUIRE) != RB_MAGIC) {
        memset(s, 0, RB_SHM_SIZE);
        s->ctrl.version = RB_VERSION;
        __atomic_store_n(&s->ctrl.magic, RB_MAGIC, __ATOMIC_RELEASE);
    }

    /* 非创建方等一会儿，让创建方把魔数写进去 */
    if (!create) {
        for (int i = 0; i < 200; i++) {
            if (__atomic_load_n(&s->ctrl.magic, __ATOMIC_ACQUIRE) == RB_MAGIC)
                break;
            usleep(10000);
        }
    }

    t = calloc(1, sizeof(*t));
    p = calloc(1, sizeof(*p));
    if (!t || !p) {
        munmap(s, RB_SHM_SIZE);
        close(fd);
        free(t);
        free(p);
        return NULL;
    }
    p->fd = fd;
    snprintf(p->name, sizeof(p->name), "%s", name);

    t->name = "shm";
    t->shm = s;
    t->priv = p;
    t->notify = shm_notify;
    t->wait = shm_wait;
    t->close = shm_close;
    return t;
}
