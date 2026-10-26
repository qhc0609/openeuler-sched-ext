/*
 * transport_ivshmem.c — 载体 B：Jailhouse 的 ivshmem
 *
 * 【重要】这个后端没有在本仓库的测试环境里验证过，也验证不了。
 * 原因写在 jailhouse/README.md：Jailhouse 要求 CPU 暴露 vmx/svm 并由它独占
 * 物理核与内存映射，而当前开发用的 VMware 虚拟机两者都没有。
 * 这段代码是按 Jailhouse 的 ivshmem 约定写的，上裸机后需要现场调试。
 *
 * ivshmem 的约定（见 Jailhouse 官方 inmates/ivshmem-demo）：
 *   BAR0  中断/状态寄存器（doorbell 写这里，对端收到中断）
 *   BAR1  保留
 *   BAR2  共享内存本体，两个 cell 映射的是同一段物理内存
 *
 * 所以这里做的事和 transport_shm.c 一模一样：映射一段共享内存，按门铃。
 * 唯一区别是内存来自 PCI BAR 而不是 /dev/shm，门铃是写寄存器而不是 futex。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

struct ivshm_priv {
    int fd_mem;      /* BAR2 */
    int fd_door;     /* BAR0，可能打不开 */
    void *bar0;
    size_t bar0_len;
    char bdf[256];
};

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int ivshm_notify(struct rb_transport *t, int ch)
{
    struct ivshm_priv *p = t->priv;

    /* 共享内存里的计数一定要加：对端即使收不到中断，轮询也能看到 */
    rb_doorbell(t->shm, ch);

    /* 有 BAR0 就再写一次寄存器，触发对端中断 */
    if (p->bar0 && (size_t)(ch + 1) * 4 <= p->bar0_len)
        *(volatile uint32_t *)((char *)p->bar0 + ch * 4) = 1;
    return 0;
}

static int ivshm_wait(struct rb_transport *t, int ch, int timeout_ms)
{
    uint32_t seen = __atomic_load_n(&t->shm->doorbell.v[ch], __ATOMIC_ACQUIRE);
    long deadline = now_ms() + timeout_ms;

    /*
     * 裸机 inmate 通常没有可用的阻塞原语，这里用轮询。
     * 1 kHz 控制环本来就要醒，轮询的代价可以接受。
     */
    while (now_ms() < deadline) {
        if (__atomic_load_n(&t->shm->doorbell.v[ch], __ATOMIC_ACQUIRE) != seen)
            return 0;
        usleep(50);
    }
    return 1;
}

static void ivshm_close(struct rb_transport *t)
{
    struct ivshm_priv *p = t->priv;

    if (t->shm && t->shm != MAP_FAILED)
        munmap(t->shm, RB_SHM_SIZE);
    if (p) {
        if (p->bar0 && p->bar0 != MAP_FAILED)
            munmap(p->bar0, p->bar0_len);
        if (p->fd_mem >= 0)
            close(p->fd_mem);
        if (p->fd_door >= 0)
            close(p->fd_door);
        free(p);
    }
    free(t);
}

struct rb_transport *rb_ivshmem_open(const char *bdf, int create)
{
    struct rb_transport *t;
    struct ivshm_priv *p;
    struct rb_shared *s;
    char path[512];
    int fd_mem;

    /* BAR2 = 共享内存 */
    snprintf(path, sizeof(path), "%s/resource2", bdf);
    fd_mem = open(path, O_RDWR | O_SYNC);
    if (fd_mem < 0) {
        fprintf(stderr,
                "打不开 %s：%s\n"
                "  ivshmem 设备不存在。这个后端只能在裸机 + Jailhouse 上用，\n"
                "  当前环境（虚拟机）没有 ivshmem。改用 shm: 后端。\n",
                path, strerror(errno));
        return NULL;
    }

    s = mmap(NULL, RB_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mem, 0);
    if (s == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
        close(fd_mem);
        return NULL;
    }

    if (create && __atomic_load_n(&s->ctrl.magic, __ATOMIC_ACQUIRE) != RB_MAGIC) {
        memset(s, 0, RB_SHM_SIZE);
        s->ctrl.version = RB_VERSION;
        __atomic_store_n(&s->ctrl.magic, RB_MAGIC, __ATOMIC_RELEASE);
    }

    t = calloc(1, sizeof(*t));
    p = calloc(1, sizeof(*p));
    if (!t || !p) {
        munmap(s, RB_SHM_SIZE);
        close(fd_mem);
        free(t);
        free(p);
        return NULL;
    }
    p->fd_mem = fd_mem;
    p->fd_door = -1;
    p->bar0 = NULL;
    snprintf(p->bdf, sizeof(p->bdf), "%s", bdf);

    /* BAR0 = doorbell，打不开不致命，退化成轮询 */
    snprintf(path, sizeof(path), "%s/resource0", bdf);
    p->fd_door = open(path, O_RDWR | O_SYNC);
    if (p->fd_door >= 0) {
        p->bar0_len = 4096;
        p->bar0 = mmap(NULL, p->bar0_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED, p->fd_door, 0);
        if (p->bar0 == MAP_FAILED)
            p->bar0 = NULL;
    }
    if (!p->bar0)
        fprintf(stderr, "警告：BAR0 打不开，doorbell 退化为共享内存轮询\n");

    t->name = "ivshmem";
    t->shm = s;
    t->priv = p;
    t->notify = ivshm_notify;
    t->wait = ivshm_wait;
    t->close = ivshm_close;
    return t;
}
