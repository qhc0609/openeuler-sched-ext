/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minimal.c — minimal.bpf.c 的用户态加载器
 *
 * 它做四件事：
 *   1. 打开编译好的 minimal.bpf.o
 *   2. 把它加载进内核（这一步会过 verifier 和 BTF 类型检查）
 *   3. 挂上 struct_ops，调度器开始接管 CPU
 *   4. 每秒读一次统计 map，让你看见它真的在干活
 *
 * 退出方式：Ctrl+C。进程一退，sched_ext 自动注销，
 * CPU 回到 CFS —— 不需要重启，也不需要改任何配置。
 *
 * 编译：make
 * 运行：sudo ./minimal ./minimal.bpf.o
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

/* Ctrl+C 时置位，主循环看到就退出 */
static volatile sig_atomic_t exiting;

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

/*
 * 读统计 map 并打印。
 *
 * 因为 map 是 PERCPU_ARRAY，每个 CPU 一份值，
 * bpf_map_lookup_elem 会把所有 CPU 的值一次性拷出来（长度 = CPU 数），
 * 所以这里要把它们加起来才是总数。
 */
static void print_stats(int map_fd, int nr_cpus)
{
	__u64 *vals = calloc(nr_cpus, sizeof(__u64));
	__u64 local = 0, shared = 0;

	if (!vals)
		return;

	for (__u32 key = 0; key < 2; key++) {
		__u64 sum = 0;

		if (bpf_map_lookup_elem(map_fd, &key, vals) != 0)
			continue;
		for (int i = 0; i < nr_cpus; i++)
			sum += vals[i];
		if (key == 0)
			local = sum;
		else
			shared = sum;
	}

	printf("local=%-8llu shared=%-8llu\n",
	       (unsigned long long)local, (unsigned long long)shared);
	fflush(stdout);
	free(vals);
}

int main(int argc, char **argv)
{
	const char *obj_path = (argc > 1) ? argv[1] : "minimal.bpf.o";
	struct bpf_object *obj = NULL;
	struct bpf_map *ops_map = NULL;
	struct bpf_link *link = NULL;
	int stats_fd = -1;
	int err;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/*
	 * 第一步：打开。
	 * 这一步只是把 ELF 读进内存，解析段和 BTF，还不碰内核。
	 */
	obj = bpf_object__open_file(obj_path, NULL);
	if (!obj) {
		fprintf(stderr, "打不开 %s: %s\n", obj_path, strerror(errno));
		return 1;
	}

	/*
	 * 第二步：加载。
	 * 这是最容易失败的一步，常见原因：
	 *   - verifier 拒绝（循环没上界、指针越界、调了不在白名单的 kfunc）
	 *   - BTF 类型不匹配（你的回调签名跟内核 struct sched_ext_ops 对不上）
	 *   - 内核没开 CONFIG_SCHED_CLASS_EXT
	 * 失败时 libbpf 会把 verifier 的完整日志打到 stderr，别忽略它。
	 */
	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "加载失败: %s\n", strerror(-err));
		fprintf(stderr, "检查：uname -r 是否 >= 6.12，"
				"/sys/kernel/sched_ext 是否存在\n");
		goto out;
	}

	/*
	 * 第三步：找到 struct_ops map 并挂上。
	 * map 的名字来自 BPF 程序里那个变量名 —— 我们叫 minimal_ops。
	 * 挂上之后，调度器立刻开始接管，不需要重启，也不需要额外命令。
	 */
	ops_map = bpf_object__find_map_by_name(obj, "minimal_ops");
	if (!ops_map) {
		fprintf(stderr, "找不到 minimal_ops\n");
		goto out;
	}

	link = bpf_map__attach_struct_ops(ops_map);
	if (!link) {
		fprintf(stderr, "挂载 struct_ops 失败: %s\n", strerror(errno));
		goto out;
	}

	/* 确认一下内核那边真的认了 */
	printf("调度器已挂载。查看接管状态：\n");
	printf("  cat /sys/kernel/sched_ext/root/ops\n");
	printf("Ctrl+C 退出（退出后 CPU 自动回到 CFS）\n\n");

	stats_fd = bpf_object__find_map_fd_by_name(obj, "stats");
	if (stats_fd < 0) {
		fprintf(stderr, "找不到 stats map\n");
		goto out;
	}

	/*
	 * 第四步：每秒打印一次计数。
	 * 数字在涨 = 内核真的在调用我们的 enqueue/select_cpu。
	 */
	while (!exiting) {
		print_stats(stats_fd, libbpf_num_possible_cpus());
		sleep(1);
	}

	printf("\n收到退出信号，注销调度器...\n");

out:
	/*
	 * 销毁顺序很重要：先解挂 link，再关 object。
	 * link 一销毁，内核就把 sched_ext 注销，任务全部回到 CFS。
	 */
	if (link)
		bpf_link__destroy(link);
	if (obj)
		bpf_object__close(obj);

	return err ? 1 : 0;
}
