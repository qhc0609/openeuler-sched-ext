/*
 * 版本四：手写 AVX2。
 *
 * 和版本三的差别只有两点：一次算 8 列而不是等编译器决定，以及把 A 的
 * 那个标量用广播指令送进向量寄存器。剩下的思路完全一样。
 *
 * 编译：-O2 -mavx2 -mfma -fno-tree-vectorize
 */
#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "gemm.h"

void gemm_avx2(const float *A, const float *B, float *C, int n)
{
    memset(C, 0, (size_t)n * (size_t)n * sizeof(float));

    for (int i = 0; i < n; i++) {
        int j = 0;

        for (; j + 8 <= n; j += 8) {
            __m256 acc = _mm256_setzero_ps();

            for (int k = 0; k < n; k++) {
                __m256 a = _mm256_set1_ps(A[(size_t)i * n + k]);
                __m256 b = _mm256_loadu_ps(B + (size_t)k * n + j);

                acc = _mm256_fmadd_ps(a, b, acc);
            }
            _mm256_storeu_ps(C + (size_t)i * n + j, acc);
        }

        /* 尾部不足 8 列的，退回标量 */
        for (; j < n; j++) {
            float s = 0.0f;

            for (int k = 0; k < n; k++)
                s += A[(size_t)i * n + k] * B[(size_t)k * n + j];
            C[(size_t)i * n + j] = s;
        }
    }
}
