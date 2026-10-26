/*
 * transport.c — 按 spec 前缀挑后端
 *
 * 上层（大脑、小脑）只调 rb_transport_open()，不知道底下是共享内存还是 ivshmem。
 * 换载体时只改命令行参数，二进制不用重编。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "transport.h"

struct rb_transport *rb_shm_open(const char *name, int create);
struct rb_transport *rb_ivshmem_open(const char *bdf, int create);

struct rb_transport *rb_transport_open(const char *spec, int create)
{
    if (!spec || !*spec) {
        fprintf(stderr, "传输 spec 不能为空\n");
        return NULL;
    }

    if (strncmp(spec, "shm:", 4) == 0)
        return rb_shm_open(spec + 4, create);

    if (strncmp(spec, "ivshmem:", 8) == 0)
        return rb_ivshmem_open(spec + 8, create);

    fprintf(stderr,
            "看不懂的 spec：%s\n"
            "  可用：shm:<名字>  或  ivshmem:<PCI 设备路径>\n", spec);
    return NULL;
}

int rb_transport_ready(struct rb_transport *t)
{
    if (!t || !t->shm)
        return 0;
    return __atomic_load_n(&t->shm->ctrl.magic, __ATOMIC_ACQUIRE) == RB_MAGIC;
}
