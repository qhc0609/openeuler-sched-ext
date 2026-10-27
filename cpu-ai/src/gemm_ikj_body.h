/*
 * 版本二、三共用的函数体：只把循环顺序从 i,j,k 换成 i,k,j。
 *
 * 内层 j 现在同时顺序访问 C 的一行和 B 的一行，两个都是连续读，
 * 一个 64 字节缓存行能装满 16 个 float，全用上。
 *
 * 同一份代码编译两次：
 *   -O2 -fno-tree-vectorize  → 只体现"顺序"的作用
 *   -O3 -march=native        → 顺序 + 编译器自动向量化
 * 这样两个因素才分得开。
 */
#ifndef GEMM_IKJ_BODY_H
#define GEMM_IKJ_BODY_H

#include <stddef.h>
#include <string.h>

static void gemm_ikj_impl(const float *A, const float *B, float *C, int n)
{
    memset(C, 0, (size_t)n * (size_t)n * sizeof(float));

    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            const float a = A[(size_t)i * n + k];

            for (int j = 0; j < n; j++)
                C[(size_t)i * n + j] += a * B[(size_t)k * n + j];
        }
}

#endif
