/*
 * brain.c — 大脑：看、想、规划，然后把"意图"交给小脑
 *
 * 大脑做的事：
 *   1. 感知：读小脑回写的状态，知道末端现在在哪（相当于视觉/编码器反馈）
 *   2. 决策：沿一个圆形轨迹走，遇到障碍物就绕开（势场法）
 *   3. 规划：把未来 200 ms 的笛卡尔路径按 10 ms 采样，逐点做逆解
 *   4. 下发：打成一条轨迹片段，带"从什么时候开始生效"的时间戳
 *
 * 大脑不做什么：不直接控制电机。它只给目标，多快走到、走不走得动是小脑的事。
 * 这条边界是整个架构的关键——大脑卡一下，机器人不能跟着抖。
 *
 * 想换成真模型：把 plan_path() 换掉就行。接口是
 *   给一个起始时刻和一个时长，填 n 个笛卡尔路点。里面用势场还是用 VLA 模型，
 *   对小脑来说没有区别。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

#define L1 0.30f
#define L2 0.25f

#define Q1_MIN (-2.967f)   /* ±170° */
#define Q1_MAX (2.967f)
#define Q2_MIN (-2.618f)   /* ±150° */
#define Q2_MAX (2.618f)

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

/* 二连杆逆解，肘上解。返回 -1 表示目标不可达。 */
static int ik(float x, float y, float *q1, float *q2)
{
    float c2 = (x * x + y * y - L1 * L1 - L2 * L2) / (2.0f * L1 * L2);
    float s2, t2;

    if (c2 > 1.0f || c2 < -1.0f)
        return -1;
    t2 = acosf(c2);
    s2 = sinf(t2);
    *q2 = t2;
    *q1 = atan2f(y, x) - atan2f(L2 * s2, L1 + L2 * c2);
    return 0;
}

/* 正解，给日志和误差计算用 */
static void fk(float q1, float q2, float *x, float *y)
{
    *x = L1 * cosf(q1) + L2 * cosf(q1 + q2);
    *y = L1 * sinf(q1) + L2 * sinf(q1 + q2);
}

/*
 * 规划：填 n 个路点。这是"大脑"里唯一需要被替换的函数。
 *
 * 现在做的是：末端沿半径 R 的圆走，同时被一个圆形障碍物排斥。
 * 势场法是个老实的选择——它没有学习成分，但它是真的在做避障决策，
 * 而且完全确定性，测试结果不会因为随机性飘。
 */
struct plan_ctx {
    float cx, cy, radius;    /* 圆的圆心和半径 */
    float ox, oy, orad;      /* 障碍物 */
    float omega;             /* 角速度 rad/s */
    uint64_t t_start;
};

static void plan_path(const struct plan_ctx *c, uint64_t t_ns,
                      float *px, float *py)
{
    float t = (float)(t_ns - c->t_start) / 1e9f;
    float ang = c->omega * t;
    float x = c->cx + c->radius * cosf(ang);
    float y = c->cy + c->radius * sinf(ang);
    float dx = x - c->ox, dy = y - c->oy;
    float d = sqrtf(dx * dx + dy * dy);
    float margin = c->orad + 0.06f;   /* 留 6 cm 安全边 */

    /* 离障碍物太近，沿径向推开。越近推得越狠，但最多推 8 cm。 */
    if (d < margin && d > 1e-4f) {
        float push = (margin - d) / margin;
        push = push > 1.0f ? 1.0f : push;
        x += dx / d * push * 0.08f;
        y += dy / d * push * 0.08f;
    }
    *px = x;
    *py = y;
}

static void busy_wait_us(long us)
{
    uint64_t until;

    if (us <= 0)
        return;
    until = now_ns() + (uint64_t)us * 1000ull;
    while (now_ns() < until)
        ;
}

static void usage(void)
{
    fprintf(stderr,
            "用法: brain [选项]\n"
            "  --transport SPEC   默认 shm:/robot-brain\n"
            "  --hz N             规划频率，默认 20\n"
            "  --think-us N       模拟推理耗时（忙等微秒），默认 0\n"
            "  --lead-ms N        轨迹提前量，默认 60\n"
            "  --duration-s N     跑多久，0 表示一直跑\n");
}

int main(int argc, char **argv)
{
    const char *spec = "shm:/robot-brain";
    int hz = 20;
    long think_us = 0;
    long lead_ms = 60;
    int duration_s = 0;
    struct rb_transport *t;
    struct rb_shared *s;
    struct plan_ctx ctx;
    uint64_t period_ns, next_tick, t_start;
    uint32_t seq = 0;
    long pushed = 0, failed = 0;
    struct sigaction sa;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--transport") && i + 1 < argc)
            spec = argv[++i];
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc)
            hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--think-us") && i + 1 < argc)
            think_us = atol(argv[++i]);
        else if (!strcmp(argv[i], "--lead-ms") && i + 1 < argc)
            lead_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--duration-s") && i + 1 < argc)
            duration_s = atoi(argv[++i]);
        else {
            usage();
            return 2;
        }
    }
    if (hz <= 0)
        hz = 20;
    period_ns = 1000000000ull / (uint64_t)hz;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    t = rb_transport_open(spec, 0);
    if (!t) {
        fprintf(stderr, "大脑：打不开传输 %s\n", spec);
        return 1;
    }
    s = t->shm;

    s->ctrl.mode = RB_MODE_RUN;
    __atomic_store_n(&s->ctrl.brain_alive, 1, __ATOMIC_RELEASE);

    memset(&ctx, 0, sizeof(ctx));
    ctx.cx = 0.32f;
    ctx.cy = 0.06f;
    ctx.radius = 0.11f;
    ctx.ox = 0.36f;
    ctx.oy = 0.12f;
    ctx.orad = 0.045f;
    ctx.omega = 1.1f;

    /*
     * 先等小脑报一次状态再开始规划。
     * 不等的话，第一帧轨迹是从"零位"算出来的，而机器人实际在别处，
     * 那 200 ms 里小脑会收到一个跳变——真实机器上这一下就是一次猛拽。
     */
    for (int i = 0; i < 300; i++) {
        struct rb_stat probe;

        if (rb_stat_peek_latest(s, &probe) == 0)
            break;
        usleep(10000);
    }

    t_start = now_ns();
    ctx.t_start = t_start;

    fprintf(stderr,
            "大脑启动：%d Hz，推理 %ld us，提前量 %ld ms，传输 %s\n",
            hz, think_us, lead_ms, spec);

    next_tick = t_start;
    while (!g_stop) {
        struct rb_stat st;
        struct rb_traj_seg seg;
        float q1_now = 0.0f, q2_now = 0.0f;
        uint64_t t_begin, t_decide, t0;
        int have_state;

        next_tick += period_ns;
        sleep_until(next_tick);
        if (g_stop)
            break;

        t_begin = now_ns();
        have_state = rb_stat_peek_latest(s, &st) == 0;
        if (have_state) {
            q1_now = st.q1;
            q2_now = st.q2;
        }

        /* —— 推理/规划耗时 —— */
        busy_wait_us(think_us);

        t_decide = now_ns();
        t0 = t_decide + (uint64_t)lead_ms * 1000000ull;

        memset(&seg, 0, sizeof(seg));
        seg.t0_ns = t0;
        seg.t_gen_ns = t_decide;
        seg.n = RB_TRAJ_POINTS;
        seg.seq = seq;

        for (uint32_t i = 0; i < RB_TRAJ_POINTS; i++) {
            uint64_t ti = t0 + (uint64_t)i * RB_TRAJ_DT_NS;
            float px, py, q1, q2;

            plan_path(&ctx, ti, &px, &py);
            if (ik(px, py, &q1, &q2) < 0) {
                /* 不可达：保持当前角度，让轨迹自然停住 */
                q1 = q1_now;
                q2 = q2_now;
            }
            q1 = clampf(q1, Q1_MIN, Q1_MAX);
            q2 = clampf(q2, Q2_MIN, Q2_MAX);

            /*
             * 头几个点从"实际在哪"平滑过渡到"规划在哪"。
             * 不做这一步，机器人一旦落后，每个片段开头都会有一次跳变。
             */
            if (i < 5 && have_state) {
                float w = (float)i / 5.0f;

                q1 = q1_now * (1.0f - w) + q1 * w;
                q2 = q2_now * (1.0f - w) + q2 * w;
            }
            seg.p[i].q1 = q1;
            seg.p[i].q2 = q2;
        }

        /* 速度用有限差分，最后一个点沿用前一段 */
        for (uint32_t i = 0; i + 1 < RB_TRAJ_POINTS; i++) {
            seg.p[i].dq1 = (seg.p[i + 1].q1 - seg.p[i].q1) / 0.01f;
            seg.p[i].dq2 = (seg.p[i + 1].q2 - seg.p[i].q2) / 0.01f;
        }
        seg.p[RB_TRAJ_POINTS - 1].dq1 = seg.p[RB_TRAJ_POINTS - 2].dq1;
        seg.p[RB_TRAJ_POINTS - 1].dq2 = seg.p[RB_TRAJ_POINTS - 2].dq2;

        if (rb_cmd_push(s, &seg) == 0) {
            pushed++;
            __atomic_add_fetch(&s->ctrl.brain_hb, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&s->ctrl.brain_seq, (uint64_t)seq, __ATOMIC_RELEASE);
            t->notify(t, RB_CH_CMD);
        } else {
            /* 队列满：说明小脑没在消费。记下来，不阻塞。 */
            failed++;
        }
        seq++;

        if (seq % (uint32_t)hz == 0) {
            float ex, ey;
            uint64_t took = now_ns() - t_begin;

            fk(seg.p[0].q1, seg.p[0].q2, &ex, &ey);
            fprintf(stderr,
                    "[brain] t=%3us  seq=%5u  目标=(%.3f,%.3f)  q=(%.2f,%.2f)  "
                    "本轮耗时=%llu us  已发=%ld 失败=%ld\n",
                    seq / (uint32_t)hz, seq, ex, ey,
                    seg.p[0].q1, seg.p[0].q2,
                    (unsigned long long)(took / 1000), pushed, failed);
        }

        if (duration_s > 0 && seq >= (uint32_t)(duration_s * hz))
            break;
    }

    s->ctrl.mode = RB_MODE_DONE;
    __atomic_store_n(&s->ctrl.brain_alive, 0, __ATOMIC_RELEASE);
    fprintf(stderr, "大脑退出：发出 %ld 条轨迹，丢弃 %ld 条\n", pushed, failed);
    t->close(t);
    return 0;
}
