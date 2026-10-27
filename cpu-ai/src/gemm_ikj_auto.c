/* 版本三：同一份代码，交给编译器自动向量化。编译：-O3 -march=native */
#include "gemm.h"
#include "gemm_ikj_body.h"

void gemm_ikj_auto(const float *A, const float *B, float *C, int n)
{
    gemm_ikj_impl(A, B, C, n);
}
