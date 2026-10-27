/* 版本二：只换循环顺序，不让编译器向量化。编译：-O2 -fno-tree-vectorize */
#include "gemm.h"
#include "gemm_ikj_body.h"

void gemm_ikj_scalar(const float *A, const float *B, float *C, int n)
{
    gemm_ikj_impl(A, B, C, n);
}
