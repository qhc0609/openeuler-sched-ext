/*
 * shm_proto.h — 大脑与小脑之间唯一的契约
 *
 * 这个文件是整件事的地基：大脑、小脑、两个传输后端都只认这里定义的布局。
 * 只要布局不变，底下是 Linux 共享内存还是 Jailhouse 的 ivshmem，上层代码一行不用改。
 *
 * 布局原则：
 *   1. 所有跨进程字段都用 __atomic 读写，不靠 volatile 保证顺序。
 *   2. 生产者写的索引和消费者写的索引分在不同缓存行，避免伪共享。
 *   3. 时间戳一律用小脑的单调时钟。大脑发轨迹时带"这条轨迹从什么时候开始生效"，
 *      由小脑按自己的钟判断，不依赖两边时钟同步。
 *   4. 环形队列是单生产者单消费者（SPSC），所以不需要锁，也不需要 CAS 循环。
 */
#ifndef RB_SHM_PROTO_H
#define RB_SHM_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define RB_MAGIC        0x52424E31u   /* "RBN1" */
#define RB_VERSION      1u
#define RB_CACHELINE    64
#define RB_SHM_SIZE     (64 * 1024)

/* 门铃通道 */
#define RB_CH_CMD       0   /* 新轨迹到了 */
#define RB_CH_ESTOP     1   /* 急停，优先级最高 */

/* 环形队列槽数，必须是 2 的幂 */
#define RB_NSLOTS_CMD   8
#define RB_NSLOTS_STAT  64

/* 一条轨迹片段：21 个点，间隔 10 ms，覆盖 200 ms */
#define RB_TRAJ_POINTS  21
#define RB_TRAJ_DT_NS   10000000ull

/* 小脑状态机 */
#define RB_ST_IDLE      0
#define RB_ST_RUN       1
#define RB_ST_HOLD      2   /* 大脑失联，沿最后速度减速到零后保持 */
#define RB_ST_SAFE      3   /* 安全停机 */
#define RB_ST_FAULT     4   /* 越限或轨迹非法 */

/* 大脑模式 */
#define RB_MODE_BOOT    0
#define RB_MODE_RUN     1
#define RB_MODE_DONE    2

/* 一个轨迹点：关节角 + 关节角速度（rad, rad/s） */
struct rb_traj_point {
    float q1, q2;
    float dq1, dq2;
};

/*
 * 一条轨迹片段。
 * t0_ns 是第一个点应当被执行的时刻，用小脑的单调时钟表示。
 * 大脑算完立刻发，t0 定在"现在 + lead"，lead 就是留给传输和调度的余量。
 */
struct rb_traj_seg {
    uint64_t t0_ns;
    uint64_t t_gen_ns;   /* 大脑做完决策的时刻，用来算端到端延迟 */
    uint32_t n;
    uint32_t seq;
    struct rb_traj_point p[RB_TRAJ_POINTS];
};

/* 小脑每周期回写一帧状态，大脑靠它知道机器人现在在哪 */
struct rb_stat {
    uint64_t t_ns;
    float q1, q2;
    float dq1, dq2;
    float ee_x, ee_y;      /* 末端位置，给大脑做规划用 */
    float track_err;       /* 指令与实际的角度差，反映伺服跟随情况 */
    uint32_t period_ns;    /* 本周期实际长度，抖动就看这个 */
    uint32_t state;        /* RB_ST_* */
    uint32_t miss;         /* 累计超时周期数 */
    uint32_t wd;           /* 累计看门狗触发次数 */
    uint32_t pad[7];
};

/*
 * 共享内存总布局。
 * 每个会被两个进程同时写的索引单独占一条缓存行——这是无锁队列不抖的关键。
 */
struct rb_shared {
    struct {
        uint32_t magic;
        uint32_t version;
        uint32_t mode;          /* 大脑写 */
        uint32_t brain_alive;   /* 大脑写，退出前置 0 */
        uint64_t brain_seq;     /* 大脑发了几条轨迹 */
        uint64_t brain_hb;      /* 大脑心跳，每轮 +1 */
        uint64_t estop;         /* 非 0 表示急停 */
        uint32_t stat_drop;     /* 小脑因状态队列满丢弃的帧数 */
        uint32_t pad;
    } ctrl __attribute__((aligned(RB_CACHELINE)));

    struct { uint32_t head; uint32_t pad[15]; } cmd_prod __attribute__((aligned(RB_CACHELINE)));
    struct { uint32_t tail; uint32_t pad[15]; } cmd_cons __attribute__((aligned(RB_CACHELINE)));
    struct { uint32_t head; uint32_t pad[15]; } stat_prod __attribute__((aligned(RB_CACHELINE)));
    struct { uint32_t tail; uint32_t pad[15]; } stat_cons __attribute__((aligned(RB_CACHELINE)));

    struct { uint32_t v[2]; uint32_t pad[14]; } doorbell __attribute__((aligned(RB_CACHELINE)));

    struct rb_traj_seg cmd[RB_NSLOTS_CMD] __attribute__((aligned(RB_CACHELINE)));
    struct rb_stat     stat[RB_NSLOTS_STAT] __attribute__((aligned(RB_CACHELINE)));

    /* 小脑写，大脑和小脑都能读，只用于出报告 */
    struct {
        uint64_t samples;          /* 控制周期总数 */
        uint64_t miss;             /* 超过 1.5 倍周期的次数 */
        uint64_t wd;               /* 看门狗触发次数 */
        uint64_t estop_cnt;
        uint64_t period_max_ns;
        uint64_t period_min_ns;
        uint64_t period_sum_ns;
        uint64_t lat_sum_us;       /* 端到端延迟累加 */
        uint64_t lat_max_us;
        uint64_t lat_samples;
        uint64_t traj_consumed;    /* 消费了几条轨迹 */
        uint64_t traj_stale;       /* 因太晚而丢弃的轨迹条数 */
    } metrics __attribute__((aligned(RB_CACHELINE)));
};

/* ---------------- 无锁 SPSC 队列 ---------------- */

/* 大脑 → 小脑：推一条轨迹。满了返回 -1，调用方应当记录并丢弃旧数据。 */
static inline int rb_cmd_push(struct rb_shared *s, const struct rb_traj_seg *seg)
{
    uint32_t h = __atomic_load_n(&s->cmd_prod.head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&s->cmd_cons.tail, __ATOMIC_ACQUIRE);

    if (h - t >= RB_NSLOTS_CMD)
        return -1;
    s->cmd[h & (RB_NSLOTS_CMD - 1)] = *seg;
    __atomic_store_n(&s->cmd_prod.head, h + 1, __ATOMIC_RELEASE);
    return 0;
}

static inline int rb_cmd_pop(struct rb_shared *s, struct rb_traj_seg *out)
{
    uint32_t t = __atomic_load_n(&s->cmd_cons.tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&s->cmd_prod.head, __ATOMIC_ACQUIRE);

    if (t == h)
        return -1;
    *out = s->cmd[t & (RB_NSLOTS_CMD - 1)];
    __atomic_store_n(&s->cmd_cons.tail, t + 1, __ATOMIC_RELEASE);
    return 0;
}

/* 小脑 → 大脑：回一帧状态。满了直接丢，状态旧了没用，不值得等。 */
static inline int rb_stat_push(struct rb_shared *s, const struct rb_stat *st)
{
    uint32_t h = __atomic_load_n(&s->stat_prod.head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&s->stat_cons.tail, __ATOMIC_ACQUIRE);

    if (h - t >= RB_NSLOTS_STAT)
        return -1;
    s->stat[h & (RB_NSLOTS_STAT - 1)] = *st;
    __atomic_store_n(&s->stat_prod.head, h + 1, __ATOMIC_RELEASE);
    return 0;
}

/* 大脑读最新一帧，不消费——规划只关心"现在在哪"，不需要历史。 */
static inline int rb_stat_peek_latest(struct rb_shared *s, struct rb_stat *out)
{
    uint32_t h = __atomic_load_n(&s->stat_prod.head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&s->stat_cons.tail, __ATOMIC_ACQUIRE);

    if (h == t)
        return -1;
    *out = s->stat[(h - 1) & (RB_NSLOTS_STAT - 1)];
    /* 大脑推进 tail，让队列不会假满。丢掉的旧状态本来就没人要。 */
    __atomic_store_n(&s->stat_cons.tail, h, __ATOMIC_RELEASE);
    return 0;
}

static inline void rb_doorbell(struct rb_shared *s, int ch)
{
    __atomic_add_fetch(&s->doorbell.v[ch], 1, __ATOMIC_RELEASE);
}

#endif /* RB_SHM_PROTO_H */
