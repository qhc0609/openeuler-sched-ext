#ifndef GEMM_H
#define GEMM_H

/*
 * 四个版本的矩阵乘，接口一样，只有实现不同。
 * 分开编译是为了让每个版本用各自的编译选项，这样"慢在哪"才归因得清楚。
 */
void gemm_ijk(const float *A, const float *B, float *C, int n);        /* 教科书三重循环 */
void gemm_ikj_scalar(const float *A, const float *B, float *C, int n);  /* 只换循环顺序 */
void gemm_ikj_auto(const float *A, const float *B, float *C, int n);    /* 顺序 + 编译器自动向量化 */
void gemm_avx2(const float *A, const float *B, float *C, int n);       /* 手写 AVX2，顺序还是错的 */
void gemm_avx2_ikj(const float *A, const float *B, float *C, int n);   /* 手写 AVX2，顺序也对 */

#endif
