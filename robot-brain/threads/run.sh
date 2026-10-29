#!/bin/sh
# run.sh —— 按文章里的顺序把三个程序跑一遍
#
# 注意日志写到 /root，不要写 /tmp。/tmp 在这台机器上是 tmpfs，
# fsync 什么都不做，量出来的数是假的。
set -e
cd "$(dirname "$0")"
make -s

echo "###############################################################"
echo "# 1. 一个循环干四件事"
echo "###############################################################"
./bin/blocked /root/rb-naive.log

echo "###############################################################"
echo "# 2. 拆成四个线程：传感器 2 kHz，控制 1 kHz"
echo "###############################################################"
echo
echo "--- 2.1 每拍只取一帧（队列里排着 32 ms 的数据）---"
./bin/split --rate 2 --drain 0 --q mutex --cpus 4 --secs 5 --log /root/rb-a.log
echo "--- 2.2 每拍排空队列，只留最新一帧 ---"
./bin/split --rate 2 --drain 1 --q mutex --cpus 4 --secs 5 --log /root/rb-b.log

echo "###############################################################"
echo "# 2.3 四个线程不绑核 / 全挤在一个核上"
echo "###############################################################"
echo "--- 2.3 不绑核，让调度器自己排 ---"
./bin/split --rate 2 --drain 1 --q mutex --cpus 0 --secs 5 --log /root/rb-i.log
echo "--- 2.4 四个线程全绑到 1 个核 ---"
./bin/split --rate 2 --drain 1 --q mutex --cpus 1 --secs 5 --log /root/rb-j.log

echo "###############################################################"
echo "# 3. 日志队列：满了丢谁"
echo "###############################################################"
echo
echo "--- 3.1 无界链表，满了不丢 ---"
./bin/split --rate 1 --drain 1 --q mutex --cpus 4 --secs 5 --log /root/rb-c.log
echo "--- 3.2 环形队列 512 条，满了丢最旧 ---"
./bin/split --rate 1 --drain 1 --q ring --drop old --cap 512 --cpus 4 --secs 5 \
	--log /root/rb-d.log
echo "--- 3.3 环形队列 512 条，满了丢最新 ---"
./bin/split --rate 1 --drain 1 --q ring --drop new --cap 512 --cpus 4 --secs 5 \
	--log /root/rb-e.log
echo "--- 3.4 环形队列 512 条，丢最新，日志线程攒 100 行 fsync 一次 ---"
./bin/split --rate 1 --drain 1 --q ring --drop new --cap 512 --batch 100 --cpus 4 \
	--secs 5 --log /root/rb-f.log

echo "###############################################################"
echo "# 4. 队列和日志的价钱"
echo "###############################################################"
./bin/bench /root/rb-bench.log
