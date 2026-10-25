/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ai_prio.c — ai_prio.bpf.c 的用户态加载器
 *
 * 比 minimal.c 多做一件事：把"优待谁、怎么优待"写进 cfg map。
 *
 * 用法：
 *   sudo ./ai_prio ./ai_prio.bpf.o <pid> [选项]
 *
 * 选项：
 *   --kick 0|1          推理任务唤醒时是否踢目标 CPU（默认 1）
 *   --burst N           推理任务最多连续占几个时间片，超过就让一次（默认 0 = 不限）
 *   --wake-boost 0|1    普通任务唤醒时插队头并踢核（默认 0）
 *   --ai-slice-us N     推理任务时间片，微秒（默认 20000 = 20 ms）
 *   --other-slice-us N  普通任务时间片，微秒（默认 3000 = 3 ms）
 *   --quiet             不打印每秒统计
 *
 * 顺序很重要：open → load → 写 cfg → attach。
 *
 * 不能"写 cfg 再 load"，因为 map 的文件描述符要到 load 之后才存在。
 * 也不能"attach 之后再写 cfg"，因为 attach 那一刻调度器就开始接管，
 * 而 target_pid 还是 0，等于有几毫秒谁都得不到优待，脏数据会混进统计。
 *
 * 所以 load 和 attach 之间那个空档正好用来写配置 —— 内核里还没人在调度，
 * 写什么都不影响任何人。
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

/* 必须和 ai_prio.bpf.c 里的 struct cfg 逐字段一致，否则写进去是垃圾 */
struct cfg {
	unsigned int target_pid;
	unsigned int kick_on_wake;
	unsigned int ai_burst;
	unsigned int wake_boost;
	unsigned long long ai_slice_ns;
	unsigned long long other_slice_ns;
};

static volatile sig_atomic_t exiting;

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: sudo %s <ai_prio.bpf.o> <pid> [选项]\n"
		"  --kick 0|1          推理任务唤醒时踢目标 CPU（默认 1）\n"
		"  --burst N           最多连续占几个时间片（默认 0 = 不限）\n"
		"  --wake-boost 0|1    普通任务唤醒时插队头并踢核（默认 0）\n"
		"  --ai-slice-us N     推理任务时间片，微秒（默认 20000）\n"
		"  --other-slice-us N  普通任务时间片，微秒（默认 3000）\n"
		"  --quiet             不打印每秒统计\n", prog);
}

static void print_stats(int stats_fd, int nr_cpus)
{
	unsigned long long *vals = calloc((size_t)nr_cpus, sizeof(unsigned long long));
	unsigned long long sum[6] = { 0, 0, 0, 0, 0, 0 };

	if (!vals)
		return;

	for (unsigned int key = 0; key < 6; key++) {
		if (bpf_map_lookup_elem(stats_fd, &key, vals) != 0)
			continue;
		for (int i = 0; i < nr_cpus; i++)
			sum[key] += vals[i];
	}

	printf("ai_enq=%-8llu other_enq=%-8llu kick=%-8llu "
	       "ai_pick=%-8llu other_pick=%-8llu boost=%-8llu\n",
	       sum[0], sum[1], sum[2], sum[3], sum[4], sum[5]);
	fflush(stdout);
	free(vals);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "kick", required_argument, NULL, 'k' },
		{ "burst", required_argument, NULL, 'b' },
		{ "wake-boost", required_argument, NULL, 'w' },
		{ "ai-slice-us", required_argument, NULL, 'a' },
		{ "other-slice-us", required_argument, NULL, 'o' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct cfg cfg = {
		.target_pid = 0,
		.kick_on_wake = 1,
		.ai_burst = 0,
		.wake_boost = 0,
		.ai_slice_ns = 20000000ULL,
		.other_slice_ns = 3000000ULL,
	};
	struct bpf_object *obj = NULL;
	struct bpf_map *ops_map = NULL;
	struct bpf_link *link = NULL;
	int cfg_fd = -1, stats_fd = -1;
	int err = 0, quiet = 0, opt;
	unsigned int key = 0;
	unsigned long target_pid;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	const char *obj_path = argv[1];

	target_pid = strtoul(argv[2], NULL, 10);
	if (!target_pid) {
		fprintf(stderr, "pid 不合法: %s\n", argv[2]);
		return 1;
	}
	cfg.target_pid = (unsigned int)target_pid;

	optind = 3;
	while ((opt = getopt_long(argc, argv, "k:b:w:a:o:qh", opts, NULL)) != -1) {
		switch (opt) {
		case 'k':
			cfg.kick_on_wake = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'b':
			cfg.ai_burst = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'w':
			cfg.wake_boost = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'a':
			cfg.ai_slice_ns = strtoull(optarg, NULL, 10) * 1000ULL;
			break;
		case 'o':
			cfg.other_slice_ns = strtoull(optarg, NULL, 10) * 1000ULL;
			break;
		case 'q':
			quiet = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* 第一步：打开。只解析 ELF 和 BTF，不碰内核。 */
	obj = bpf_object__open_file(obj_path, NULL);
	if (!obj) {
		fprintf(stderr, "打不开 %s: %s\n", obj_path, strerror(errno));
		return 1;
	}

	/* 第二步：加载。过 verifier + BTF 类型检查。 */
	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "加载失败: %s\n", strerror(-err));
		fprintf(stderr, "常见原因：内核 < 6.12、没开 CONFIG_SCHED_CLASS_EXT、"
				"或 verifier 拒绝（往上翻日志）\n");
		err = 1;
		goto out;
	}

	/* 第三步：写配置。此时调度器还没挂上，改配置不影响任何人。 */
	cfg_fd = bpf_object__find_map_fd_by_name(obj, "cfg_map");
	if (cfg_fd < 0) {
		fprintf(stderr, "找不到 cfg_map\n");
		err = 1;
		goto out;
	}
	if (bpf_map_update_elem(cfg_fd, &key, &cfg, BPF_ANY) != 0) {
		fprintf(stderr, "写 cfg_map 失败: %s\n", strerror(errno));
		err = 1;
		goto out;
	}

	/* 第四步：挂上 struct_ops。从这一行开始，CPU 由我们调度。 */
	ops_map = bpf_object__find_map_by_name(obj, "ai_prio_ops");
	if (!ops_map) {
		fprintf(stderr, "找不到 ai_prio_ops\n");
		err = 1;
		goto out;
	}

	link = bpf_map__attach_struct_ops(ops_map);
	if (!link) {
		fprintf(stderr, "挂载 struct_ops 失败: %s\n", strerror(errno));
		err = 1;
		goto out;
	}

	printf("ai_prio 已接管。target_pid=%lu kick=%u burst=%u boost=%u "
	       "ai_slice=%llums other_slice=%llums\n",
	       target_pid, cfg.kick_on_wake, cfg.ai_burst, cfg.wake_boost,
	       cfg.ai_slice_ns / 1000000ULL, cfg.other_slice_ns / 1000000ULL);
	printf("查看接管状态：cat /sys/kernel/sched_ext/root/ops\n");
	printf("Ctrl+C 退出（退出后 CPU 自动回到 CFS）\n\n");
	fflush(stdout);

	stats_fd = bpf_object__find_map_fd_by_name(obj, "stats");
	if (stats_fd < 0) {
		fprintf(stderr, "找不到 stats map\n");
		err = 1;
		goto out;
	}

	while (!exiting) {
		if (!quiet)
			print_stats(stats_fd, libbpf_num_possible_cpus());

		/*
		 * 目标进程退出了就主动收工。
		 * 否则调度器会一直挂在那儿"优待一个不存在的 pid"，
		 * 后面的实验会以为自己在测一个已经结束的进程。
		 */
		if (kill((pid_t)target_pid, 0) != 0 && errno == ESRCH) {
			printf("\ntarget pid %lu 已退出，自动注销。\n", target_pid);
			break;
		}
		sleep(1);
	}

	printf("\n收到退出信号，注销调度器...\n");

out:
	/* 先解挂，再关对象。link 一销毁，所有任务回到 CFS。 */
	if (link)
		bpf_link__destroy(link);
	if (obj)
		bpf_object__close(obj);

	return err ? 1 : 0;
}
