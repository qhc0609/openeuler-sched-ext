/*
 * transport.h — 大小脑通信的传输层
 *
 * 为什么要有这一层：
 *   "共享内存 + 门铃"这个模型，在 Linux 上是 memfd/shm + futex，
 *   在 Jailhouse 上是 ivshmem 的 BAR + doorbell 中断。
 *   上层协议（环形队列、轨迹格式、时间语义）完全一样，只有"内存从哪来"
 *   和"怎么叫醒对方"不一样。把这两件事抽出来，代码就能在两种载体上跑。
 *
 * 载体 A（今天就能测）：shm，POSIX 共享内存 + futex
 * 载体 B（需要裸机）：ivshmem，Jailhouse 的虚拟 PCI 设备
 */
#ifndef RB_TRANSPORT_H
#define RB_TRANSPORT_H

#include "shm_proto.h"

struct rb_transport {
    const char *name;
    struct rb_shared *shm;
    void *priv;

    /* 叫醒对端。返回 0 成功。 */
    int (*notify)(struct rb_transport *t, int ch);

    /* 等对端叫醒。返回 0 = 被叫醒，1 = 超时。 */
    int (*wait)(struct rb_transport *t, int ch, int timeout_ms);

    void (*close)(struct rb_transport *t);
};

/*
 * spec 写法：
 *   "shm:/robot-brain"          —— 载体 A
 *   "ivshmem:/dev/ivshmem0"     —— 载体 B
 *
 * create = 1 时由本进程负责初始化共享区（清头、写魔数）。
 * 两边都传 1 也没问题：先到的初始化，后到的看到魔数已经对就跳过。
 */
struct rb_transport *rb_transport_open(const char *spec, int create);

/* 校验共享区是否已经初始化好 */
int rb_transport_ready(struct rb_transport *t);

#endif /* RB_TRANSPORT_H */
