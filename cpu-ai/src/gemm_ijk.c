/*
 * 版本一：教科书上的三重循环。
 *
 * 写成 i、j、k 这个顺序是最自然的——"C 的第 i 行第 j 列，等于 A 的第 i 行
 * 点乘 B 的第 j 列"。但内层 k 每次都要跳 n 个 float 去取 B[k][j]，
 * 内存访问是跨行跳跃的，一个缓存行只用到 4 个字节就扔掉。
 *
 * 编译：gcc -O2 -fno-tree-vectorize
 */
#include <stddef.h>

#include "gemm.h"

void gemm_ijk(const float *A, const float *B, float *C, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float s = 0.0f;

            for (int k = 0; k < n; k++)
                s += A[(size_t)i * n + k] * B[(size_t)k * n + j];
            C[(size_t)i * n + j] = s;
        }
}
