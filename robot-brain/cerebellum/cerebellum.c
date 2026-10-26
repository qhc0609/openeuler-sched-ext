/*
 * cerebellum.c — 小脑：1 kHz 控制环，只做"这一毫秒该输出什么"
 *
 * 小脑的职责边界很清楚：
 *   收轨迹 → 按时间插值 → 限位限速 → 输出关节指令 → 回写状态
 *
 * 它不做规划，也不等大脑。大脑断了，它必须能自己安全停下来。
 * 这就是"小脑"存在的全部理由：大脑可以卡、可以死，机器人不能倒。
 *
 * 三个安全机制：
 *   1. 看门狗：超过 wd-hold-ms 没收到新轨迹，沿最后速度平滑减速到零
 *   2. 安全停机：超过 wd-safe-ms，保持位置并置 SAFE 状态
 *   3. 越限保护：关节角、角速度超限一律截断并计数
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

#define L1 0.30f
#define L2 0.25f

#define Q1_MIN (-2.967f)
#define Q1_MAX (2.967f)
#define Q2_MIN (-2.618f)
#define Q2_MAX (2.618f)
#define DQ_MAX 3.0f          /* rad/s，关节速度上限 */

#define PT_MAX 256
#define PT_MASK (PT_MAX - 1)
#define PERIOD_HIST 1000

struct pt {
    uint64_t t;
    uint64_t gen;    /* 这个点是哪一刻被大脑规划出来的 */
    float q1, q2, dq1, dq2;
};

static struct pt ptbuf[PT_MAX];
static uint64_t pt_r, pt_w;
static uint64_t pt_last_t;

static uint32_t period_hist[PERIOD_HIST];
static uint32_t period_hist_n;

static volatile sig_atomic_t g_stop;

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
    struct timespec ts = {
        .tv_sec = (time_t)(t / 1000000000ull),
        .tv_nsec = (long)(t % 1000000000ull),
    };

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void fk(float q1, float q2, float *x, float *y)
{
    *x = L1 * cosf(q1) + L2 * cosf(q1 + q2);
    *y = L1 * sinf(q1) + L2 * sinf(q1 + q2);
}

/* 追加一个路点。时间不递增的点直接丢——重叠片段靠这条去重。 */
static void pt_append(const struct pt *p)
{
    if (p->t <= pt_last_t)
        return;
    if (pt_w - pt_r >= PT_MAX)
        pt_r++;                      /* 真满了就丢最旧的 */
    ptbuf[pt_w & PT_MASK] = *p;
    pt_w++;
    pt_last_t = p->t;
}

/*
 * 在 now 时刻插值。返回 0 正常，1 表示轨迹已经用完（该看看门狗了）。
 * 用三次 Hermite：位置和速度都连续，不然关节会一顿一顿。
 */
static int pt_interp(uint64_t now, float *q1, float *q2)
{
    struct pt *a, *b;
    double u, dt;
    double h00, h10, h01, h11;

    if (pt_w == pt_r)
        return 1;

    while (pt_r + 1 < pt_w && ptbuf[(pt_r + 1) & PT_MASK].t <= now)
        pt_r++;

    a = &ptbuf[pt_r & PT_MASK];
    if (now <= a->t) {
        *q1 = a->q1;
        *q2 = a->q2;
        return 0;
    }
    if (pt_r + 1 >= pt_w) {
        *q1 = a->q1;
        *q2 = a->q2;
        return 1;
    }

    b = &ptbuf[(pt_r + 1) & PT_MASK];
    dt = (double)(b->t - a->t) / 1e9;
    u = (double)(now - a->t) / (double)(b->t - a->t);

    h00 = 2 * u * u * u - 3 * u * u + 1;
    h10 = u * u * u - 2 * u * u + u;
    h01 = -2 * u * u * u + 3 * u * u;
    h11 = u * u * u - u * u;

    *q1 = (float)(h00 * a->q1 + h10 * dt * a->dq1 + h01 * b->q1 + h11 * dt * b->dq1);
    *q2 = (float)(h00 * a->q2 + h10 * dt * a->dq2 + h01 * b->q2 + h11 * dt * b->dq2);
    return 0;
}

static uint32_t pct(const uint32_t *h, int n, int p)
{
    uint32_t tmp[PERIOD_HIST];
    int k;

    if (n <= 0)
        return 0;
    memcpy(tmp, h, sizeof(uint32_t) * (size_t)n);
    /* 插入排序，n ≤ 1000，够用且不用分配 */
    for (int i = 1; i < n; i++) {
        uint32_t v = tmp[i];
        int j = i - 1;

        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    k = (int)((long)n * p / 100);
    if (k >= n)
        k = n - 1;
    return tmp[k];
}

static void usage(void)
{
    fprintf(stderr,
            "用法: cerebellum [选项]\n"
            "  --transport SPEC    默认 shm:/robot-brain\n"
            "  --hz N              控制频率，默认 1000\n"
            "  --tau-ms N          伺服时间常数，默认 20\n"
            "  --rt PRIO           用 SCHED_FIFO，给优先级（0 表示不用）\n"
            "  --cpu N             绑到哪个核（-1 表示不绑）\n"
            "  --wd-hold-ms N      大脑失联多久进入保持，默认 250\n"
            "  --wd-safe-ms N      多久进入安全停机，默认 800\n"
            "  --duration-s N      跑多久，0 表示一直跑\n"
            "  --quiet             不打每秒摘要\n");
}

int main(int argc, char **argv)
{
    const char *spec = "shm:/robot-brain";
    int hz = 1000, rt_prio = 0, cpu = -1, duration_s = 0, quiet = 0;
    long tau_ms = 20, wd_hold_ms = 250, wd_safe_ms = 800;
    struct rb_transport *t;
    struct rb_shared *s;
    uint64_t period_ns, next_tick, t0;
    float q1 = 0.0f, q2 = 0.0f;          /* 实际角度 */
    float q1_cmd = 0.0f, q2_cmd = 0.0f;  /* 指令角度 */
    float q1_prev = 0.0f, q2_prev = 0.0f;/* 上一周期的指令，用于变化率限幅 */
    float hold_q1 = 0, hold_q2 = 0, hold_dq1 = 0, hold_dq2 = 0;
    uint64_t last_seg_ns = 0, last_stat_ns = 0;
    uint32_t state = RB_ST_IDLE;
    uint32_t prev_state = RB_ST_IDLE;
    int seen_seg = 0;   /* 大脑还没说过话之前，不算"失联" */
    uint64_t samples = 0, miss = 0, wd_cnt = 0, consumed = 0, stale = 0;
    uint64_t pmin = ~0ull, pmax = 0, psum = 0;
    uint64_t prev_begin = 0;          /* 上一拍的醒来时刻，用来算真实周期 */
    uint32_t work_max = 0;            /* 单拍执行耗时最大值 */
    uint64_t lat_sum = 0, lat_max = 0, lat_n = 0;
    int64_t buf_min_ms = (int64_t)1 << 40;   /* 轨迹缓冲余量最小值，负值代表已经断粮 */
    uint64_t sec_mark;
    float track_max = 0.0f;
    struct sigaction sa;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--transport") && i + 1 < argc)
            spec = argv[++i];
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc)
            hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tau-ms") && i + 1 < argc)
            tau_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rt") && i + 1 < argc)
            rt_prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
            cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wd-hold-ms") && i + 1 < argc)
            wd_hold_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--wd-safe-ms") && i + 1 < argc)
            wd_safe_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--duration-s") && i + 1 < argc)
            duration_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet"))
            quiet = 1;
        else {
            usage();
            return 2;
        }
    }
    if (hz <= 0)
        hz = 1000;
    period_ns = 1000000000ull / (uint64_t)hz;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 绑核要在起调度策略之前做，否则内核可能先把线程挪走 */
    if (cpu >= 0) {        cpu_set_t set;

        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0)
            fprintf(stderr, "警告：绑核到 CPU%d 失败：%s\n", cpu, strerror(errno));
    }
    if (rt_prio > 0) {
        struct sched_param sp = { .sched_priority = rt_prio };

        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
            fprintf(stderr, "警告：SCHED_FIFO 失败：%s\n", strerror(errno));
    }

    t = rb_transport_open(spec, 1);
    if (!t) {
        fprintf(stderr, "小脑：打不开传输 %s\n", spec);
        return 1;
    }
    s = t->shm;

    /*
     * 锁住内存。实时控制环最怕的是某一毫秒正好赶上缺页，
     * 一次缺页就能让周期翻几倍。锁住之后整个进程的页都在物理内存里。
     * 失败不致命，只是抖动会大一些。
     */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "警告：mlockall 失败：%s\n", strerror(errno));

    /*
     * 把定时器松弛量压到 1 纳秒。内核默认允许 50 微秒的松弛，
     * 对 1 kHz 的环来说那是 5% 的周期误差，白白送掉的。
     */
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

    fprintf(stderr,
            "小脑启动：%d Hz，伺服时间常数 %ld ms，rt=%d，cpu=%d，传输 %s\n",
            hz, tau_ms, rt_prio, cpu, spec);

    t0 = now_ns();
    next_tick = t0;
    sec_mark = t0;
    last_seg_ns = t0;

    while (!g_stop) {
        struct rb_traj_seg seg;
        struct rb_stat st;
        uint64_t t_begin, t_end;
        uint32_t period_used;
        int buf_empty;

        next_tick += period_ns;
        /*
         * 已经错过这一拍就不再补跑，直接对齐到"现在 + 一个周期"。
         * 不这样处理的话，落后时 sleep 会立刻返回，后面几拍的实际长度变成几十纳秒，
         * 周期统计全是假的。更要紧的是补跑没有意义——错过的那一毫秒已经过去了。
         */
        if (now_ns() >= next_tick)
            next_tick = now_ns() + period_ns;
        sleep_until(next_tick);
        if (g_stop)
            break;

        t_begin = now_ns();

        /* —— 1. 把队列里的新轨迹全收进来 —— */
        while (rb_cmd_pop(s, &seg) == 0) {
            consumed++;
            seen_seg = 1;
            last_seg_ns = now_ns();

            if (seg.t0_ns + (uint64_t)(seg.n - 1) * RB_TRAJ_DT_NS < t_begin) {
                stale++;      /* 整条都过期了，丢掉 */
                continue;
            }
            for (uint32_t i = 0; i < seg.n; i++) {
                struct pt p;

                p.t = seg.t0_ns + (uint64_t)i * RB_TRAJ_DT_NS;
                p.gen = seg.t_gen_ns;
                p.q1 = seg.p[i].q1;
                p.q2 = seg.p[i].q2;
                p.dq1 = seg.p[i].dq1;
                p.dq2 = seg.p[i].dq2;
                pt_append(&p);
            }
        }

        /* —— 2. 急停优先级最高 —— */
        if (__atomic_load_n(&s->ctrl.estop, __ATOMIC_ACQUIRE) != 0) {
            q1_cmd = q1;
            q2_cmd = q2;
            state = RB_ST_SAFE;
        } else if (!seen_seg) {
            /* 大脑一次都没说过话：待机，不报失联 */
            q1_cmd = q1;
            q2_cmd = q2;
            state = RB_ST_IDLE;
        } else if (now_ns() - last_seg_ns > (uint64_t)wd_safe_ms * 1000000ull) {
            /* —— 3. 安全停机 —— */
            if (state != RB_ST_SAFE) {
                wd_cnt++;
                fprintf(stderr, "[cerebellum] %llu ms 没收到轨迹，安全停机\n",
                        (unsigned long long)((now_ns() - last_seg_ns) / 1000000ull));
            }
            q1_cmd = q1;
            q2_cmd = q2;
            state = RB_ST_SAFE;
        } else if (now_ns() - last_seg_ns > (uint64_t)wd_hold_ms * 1000000ull) {
            /* —— 4. 大脑失联：沿最后速度平滑减速 —— */
            if (state != RB_ST_HOLD) {
                /* 沿最后一段指令速度开始减速，hold_dq 就是刚才 RUN 里算的那个 */
                hold_q1 = q1_cmd;
                hold_q2 = q2_cmd;
                state = RB_ST_HOLD;
            }
            hold_dq1 *= (1.0f - (float)period_ns / 8e7f);
            hold_dq2 *= (1.0f - (float)period_ns / 8e7f);
            hold_q1 += hold_dq1 * (float)period_ns / 1e9f;
            hold_q2 += hold_dq2 * (float)period_ns / 1e9f;
            q1_cmd = hold_q1;
            q2_cmd = hold_q2;
        } else {
            /* —— 5. 正常跟轨迹 —— */
            float nq1, nq2;

            buf_empty = pt_interp(t_begin, &nq1, &nq2);
            if (buf_empty) {
                /* 缓冲空了但还没到看门狗阈值：原地保持，速度清零 */
                q1_cmd = q1;
                q2_cmd = q2;
                hold_dq1 = hold_dq2 = 0;
                state = RB_ST_IDLE;
            } else {
                hold_dq1 = (nq1 - q1_cmd) / ((float)period_ns / 1e9f);
                hold_dq2 = (nq2 - q2_cmd) / ((float)period_ns / 1e9f);
                q1_cmd = nq1;
                q2_cmd = nq2;
                state = RB_ST_RUN;
            }
        }

        /* —— 6. 指令变化率限幅 ——
         * 不管指令从哪来（新轨迹、保持、急停），都在这里被限制成每周期最多
         * DQ_MAX * dt。没有这一层，大脑一上来给一个远处的目标，关节就会被猛拽一下。
         * 正常跟轨迹时轨迹本身的速度远低于 DQ_MAX，所以这一层不影响跟随精度。
         */
        {
            float step = DQ_MAX * (float)period_ns / 1e9f;

            q1_cmd = q1_prev + clampf(q1_cmd - q1_prev, -step, step);
            q2_cmd = q2_prev + clampf(q2_cmd - q2_prev, -step, step);
            q1_prev = q1_cmd;
            q2_prev = q2_cmd;
        }

        /* —— 7. 关节限位，超了截断并记 FAULT —— */
        {
            float lim1 = clampf(q1_cmd, Q1_MIN, Q1_MAX);
            float lim2 = clampf(q2_cmd, Q2_MIN, Q2_MAX);

            if (lim1 != q1_cmd || lim2 != q2_cmd)
                state = RB_ST_FAULT;
            q1_cmd = lim1;
            q2_cmd = lim2;

            if (hold_dq1 > DQ_MAX)
                hold_dq1 = DQ_MAX;
            if (hold_dq1 < -DQ_MAX)
                hold_dq1 = -DQ_MAX;
            if (hold_dq2 > DQ_MAX)
                hold_dq2 = DQ_MAX;
            if (hold_dq2 < -DQ_MAX)
                hold_dq2 = -DQ_MAX;
        }

        /* 状态切换单独打一行，降级测试要靠它看时间线 */
        if (state != prev_state) {
            fprintf(stderr, "[cereb] 状态切换 %u -> %u @ %llu ms\n",
                    prev_state, state,
                    (unsigned long long)((now_ns() - t0) / 1000000ull));
            prev_state = state;
        }

        /* —— 8. 伺服跟随：一阶滞后，模拟真实电机追不上指令 —— */
        {
            float k = (float)period_ns / ((float)tau_ms * 1e6f);

            if (k > 1.0f)
                k = 1.0f;
            q1 += (q1_cmd - q1) * k;
            q2 += (q2_cmd - q2) * k;
        }

        /* —— 9. 统计 ——
         * 这里要分清两个量，混在一起就看不出问题了：
         *   周期 = 这一次醒来距上一次醒来多久（1 kHz 的目标是 1 ms）
         *   耗时 = 这一拍的循环体跑了多久（必须远小于 1 ms，否则迟早超期）
         * 之前只量了耗时，看着"平均 1.3 微秒很漂亮"，其实那根本不是周期。
         */
        t_end = now_ns();
        period_used = prev_begin ? (uint32_t)(t_begin - prev_begin) : (uint32_t)period_ns;
        {
            uint32_t work_used = (uint32_t)(t_end - t_begin);

            if (work_used > work_max)
                work_max = work_used;
        }
        prev_begin = t_begin;
        samples++;
        if (period_used > (uint32_t)(period_ns * 3 / 2))
            miss++;
        if (period_used < pmin)
            pmin = period_used;
        if (period_used > pmax)
            pmax = period_used;
        psum += period_used;
        if (period_hist_n < PERIOD_HIST)
            period_hist[period_hist_n++] = period_used;

        {
            float e1 = fabsf(q1_cmd - q1);
            float e2 = fabsf(q2_cmd - q2);
            float e = e1 > e2 ? e1 : e2;

            if (e > track_max)
                track_max = e;
        }

        /* 缓冲余量：队列里还有多少毫秒的轨迹可走。降到 0 就该进入保持 */
        if (pt_w > pt_r && state == RB_ST_RUN) {
            int64_t ahead = (int64_t)(pt_last_t - t_end) / 1000000LL;

            if (ahead < buf_min_ms)
                buf_min_ms = ahead;
        }

        /* 计划年龄：大脑什么时候做完决策，这个点什么时候被真正执行完 */
        if (pt_w > pt_r && state == RB_ST_RUN) {
            uint64_t gen = ptbuf[pt_r & PT_MASK].gen;

            /*
             * gen 为 0 说明这条轨迹不是这个大脑发的（比如手工注入的）；
             * 差值离谱说明两边时钟不同源（换成 Jailhouse 之后就是这样）。
             * 两种情况都不计入，免得把平均值污染掉。
             */
            if (gen > 0 && t_end > gen && t_end - gen < 1000000000ull) {
                uint64_t lat = (t_end - gen) / 1000ull;

                lat_sum += lat;
                lat_n++;
                if (lat > lat_max)
                    lat_max = lat;
            }
        }

        /* —— 9. 每秒回写状态与摘要 —— */
        if (t_end - last_stat_ns >= 10000000ull) {   /* 100 Hz */
            float ex, ey;

            fk(q1, q2, &ex, &ey);
            memset(&st, 0, sizeof(st));
            st.t_ns = t_end;
            st.q1 = q1;
            st.q2 = q2;
            st.dq1 = hold_dq1;
            st.dq2 = hold_dq2;
            st.ee_x = ex;
            st.ee_y = ey;
            st.track_err = track_max;
            st.period_ns = period_used;
            st.state = state;
            st.miss = (uint32_t)miss;
            st.wd = (uint32_t)wd_cnt;
            if (rb_stat_push(s, &st) < 0)
                __atomic_add_fetch(&s->ctrl.stat_drop, 1, __ATOMIC_RELAXED);
            last_stat_ns = t_end;
        }

        if (!quiet && t_end - sec_mark >= 1000000000ull) {
            float ex, ey;

            fk(q1, q2, &ex, &ey);
            fprintf(stderr,
                    "[cereb] t=%3llus 状态=%u q=(%.3f,%.3f) 末端=(%.3f,%.3f) "
                    "周期 p50=%u p99=%u max=%u ns 超时=%llu 看门狗=%llu\n",
                    (unsigned long long)((t_end - t0) / 1000000000ull), state,
                    q1, q2, ex, ey,
                    pct(period_hist, (int)period_hist_n, 50),
                    pct(period_hist, (int)period_hist_n, 99),
                    (unsigned)pct(period_hist, (int)period_hist_n, 100),
                    (unsigned long long)miss, (unsigned long long)wd_cnt);
            sec_mark = t_end;
            period_hist_n = 0;   /* 每秒重新统计，反映当前状态 */
        }

        if (duration_s > 0 && (t_end - t0) / 1000000000ull >= (uint64_t)duration_s)
            break;
    }

    /* —— 收尾：把指标写进共享区，跑测试脚本的人读这里 —— */
    s->metrics.samples = samples;
    s->metrics.miss = miss;
    s->metrics.wd = wd_cnt;
    s->metrics.period_min_ns = pmin == ~0ull ? 0 : pmin;
    s->metrics.period_max_ns = pmax;
    s->metrics.period_sum_ns = psum;
    s->metrics.lat_sum_us = lat_sum;
    s->metrics.lat_max_us = lat_max;
    s->metrics.lat_samples = lat_n;
    s->metrics.traj_consumed = consumed;
    s->metrics.traj_stale = stale;

    fprintf(stderr,
            "\n小脑汇总：\n"
            "  周期数        %llu\n"
            "  周期 min/avg/max  %llu / %llu / %llu ns（两次唤醒之间的间隔）\n"
            "  单拍耗时 max  %u ns（循环体本身跑了多久）\n"
            "  超时(>1.5倍)  %llu  (%.3f%%)\n"
            "  看门狗触发    %llu\n"
            "  消费轨迹      %llu 条，过期丢弃 %llu 条\n"
            "  计划年龄      avg=%llu us  max=%llu us（大脑决策→动作执行完）\n"
            "  缓冲余量最小  %lld ms（负值代表轨迹断粮）\n"
            "  最大跟随误差  %.4f rad\n",
            (unsigned long long)samples,
            (unsigned long long)(pmin == ~0ull ? 0 : pmin),
            (unsigned long long)(samples ? psum / samples : 0),
            (unsigned long long)pmax,
            work_max,
            (unsigned long long)miss,
            samples ? 100.0 * (double)miss / (double)samples : 0.0,
            (unsigned long long)wd_cnt,
            (unsigned long long)consumed, (unsigned long long)stale,
            (unsigned long long)(lat_n ? lat_sum / lat_n : 0),
            (unsigned long long)lat_max,
            (long long)(buf_min_ms == ((int64_t)1 << 40) ? 0 : buf_min_ms),
            track_max);

    t->close(t);
    return 0;
}
