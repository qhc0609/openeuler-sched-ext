/*
 * ring.h —— 单生产者单消费者的环形队列
 *
 * 没有锁，只有两个原子下标。head 只有生产者写，tail 只有消费者写，
 * 各写各的，所以不需要互斥——这就是"单生产者单消费者"这五个字的全部
 * 价值。多一个生产者，这个结构立刻失效。
 *
 * 唯一要做的决定是：满了怎么办。两个 push 给了两个答案。
 */
#ifndef RING_H
#define RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

struct ring {
	void *buf;
	size_t cap;		/* 必须是 2 的幂，取模才能用 & (cap-1) */
	size_t esz;
	_Atomic size_t head;	/* 生产者写 */
	_Atomic size_t tail;	/* 消费者写 */
	_Atomic size_t dropped;
};

static inline void ring_init(struct ring *r, void *buf, size_t cap, size_t esz)
{
	r->buf = buf;
	r->cap = cap;
	r->esz = esz;
	atomic_store(&r->head, 0);
	atomic_store(&r->tail, 0);
	atomic_store(&r->dropped, 0);
}

static inline size_t ring_count(struct ring *r)
{
	return atomic_load_explicit(&r->head, memory_order_acquire) -
	       atomic_load_explicit(&r->tail, memory_order_acquire);
}

/*
 * 丢最旧的：把 tail 往前推一格，腾出位置。
 *
 * 消费者永远拿到最近 cap 条。控制回路要的是这个——一帧 3 秒前的激光
 * 数据，处理得再快也没用。
 */
static inline int ring_push_drop_old(struct ring *r, const void *e)
{
	size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	int lost = 0;

	if (h - t >= r->cap) {
		atomic_store_explicit(&r->tail, t + 1, memory_order_release);
		atomic_fetch_add(&r->dropped, 1);
		lost = 1;
	}
	memcpy((char *)r->buf + (h & (r->cap - 1)) * r->esz, e, r->esz);
	atomic_store_explicit(&r->head, h + 1, memory_order_release);
	return lost;
}

/*
 * 丢最新的：满了就不收。
 *
 * 消费者按顺序一条不落地处理，代价是队列里的数据越来越旧。日志要的是
 * 这个——日志少一条是丢信息，日志晚一小时是丢证据。
 */
static inline int ring_push_drop_new(struct ring *r, const void *e)
{
	size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);

	if (h - t >= r->cap) {
		atomic_fetch_add(&r->dropped, 1);
		return 0;
	}
	memcpy((char *)r->buf + (h & (r->cap - 1)) * r->esz, e, r->esz);
	atomic_store_explicit(&r->head, h + 1, memory_order_release);
	return 1;
}

static inline int ring_pop(struct ring *r, void *e)
{
	size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t h = atomic_load_explicit(&r->head, memory_order_acquire);

	if (h == t)
		return 0;
	memcpy(e, (char *)r->buf + (t & (r->cap - 1)) * r->esz, r->esz);
	atomic_store_explicit(&r->tail, t + 1, memory_order_release);
	return 1;
}

#endif
