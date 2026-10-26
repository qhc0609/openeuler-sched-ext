/*
 * spin.c — 纯 CPU 占用，用来模拟"机器上还有别的活在跑"
 *
 * 为什么不直接 yes > /dev/null：
 *   yes 每轮都要 write()，系统调用占比很高，干扰的性质变了。
 *   这里只做一个空循环，占的是纯用户态 CPU 时间。
 *
 * 用法: spin [秒数]   0 或不给表示一直转
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    volatile uint64_t x = 1;
    time_t t0 = time(NULL);

    for (;;) {
        /* 一批空转，然后才看一次时钟，避免把 time() 变成热点 */
        for (int i = 0; i < 1000000; i++)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        if (secs > 0 && time(NULL) - t0 >= secs)
            break;
    }
    return (int)(x & 1);
}
