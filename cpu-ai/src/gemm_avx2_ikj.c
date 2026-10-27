/*
 * 版本五：手写 AVX2，而且循环顺序也对。
 *
 * 和版本四的区别只有一处：k 在外面、j 在里面。
 * 于是 B 和 C 都是按行顺序访问，一次装满缓存行；
 * 版本四把 k 放在最里面，每算 8 列就要跨 n 个 float 去取 B，白搬了数据。
 *
 * 编译：-O2 -mavx2 -mfma -fno-tree-vectorize
 */
#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "gemm.h"

void gemm_avx2_ikj(const float *A, const float *B, float *C, int n)
{
    for (int i = 0; i < n; i++) {
        float *crow = C + (size_t)i * n;

        memset(crow, 0, (size_t)n * sizeof(float));

        for (int k = 0; k < n; k++) {
            const float av = A[(size_t)i * n + k];
            const __m256 a = _mm256_set1_ps(av);
            const float *brow = B + (size_t)k * n;
            int j = 0;

            for (; j + 8 <= n; j += 8) {
                __m256 c = _mm256_loadu_ps(crow + j);

                c = _mm256_fmadd_ps(a, _mm256_loadu_ps(brow + j), c);
                _mm256_storeu_ps(crow + j, c);
            }
            for (; j < n; j++)
                crow[j] += av * brow[j];
        }
    }
}
