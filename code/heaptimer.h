#ifndef HEAPTIMER_H
#define HEAPTIMER_H

#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h>
#include <functional>
#include <assert.h>
#include <chrono>
#include <vector>
#include "log.h"

//std::function<void()> 是C++11引入的 “函数包装器”
//可以存储任何无参数、无返回值的可调用对象
typedef std::function<void()> TimeoutCallBack;
//高精度时钟：用于获取当前时间，精度可达纳秒级
typedef std::chrono::high_resolution_clock Clock;
//毫秒
typedef std::chrono::milliseconds MS;
//时间点：表示具体的时刻
typedef Clock::time_point TimeStamp;

//存储单个超时事件的信息
struct TimerNode {
    int id; //事件唯一标识（通常对应客户端连接的 fd）
    TimeStamp expires; //超时时间点（绝对时间）
    TimeoutCallBack cb; //超时后要执行的回调函数（如关闭连接）
    // 重载运算符：用于堆排序（小根堆的比较规则）
    bool operator<(const TimerNode& t) const {
        return expires < t.expires; //时间早的节点“更小”，应排在堆顶
    }
    bool operator>(const TimerNode& t) const {
        return expires > t.expires; //用于判断是否需要向下调整堆
    }
};

class HeapTimer {
public:

    HeapTimer() { heap_.reserve(64); } //构造函数：预分配64个节点的空间
    ~HeapTimer() { clear(); } //析构函数：清理所有节点
    
    void adjust(int id, int newExpires); //调整id对应的超时事件的超时时间
    void add(int id, int timeOut, const TimeoutCallBack& cb); //添加新的超时事件
    void doWork(int id); //立即执行id对应的超时回调，并删除该节点
    void clear(); //清空所有超时事件
    void tick(); //处理所有已超时的事件（核心函数）
    void pop(); //删除堆顶节点（最早超时的节点）
    int GetNextTick(); //获取下一个超时事件的剩余毫秒数

private:
    void del_(size_t i); //删除堆中索引为 i 的节点
    void siftup_(size_t i); //从索引i向上调整堆
    bool siftdown_(size_t i, size_t n); //从索引i向下调整堆
    void SwapNode_(size_t i, size_t j); //交换堆中索引i和j的节点
    std::vector<TimerNode> heap_; //小根堆的底层容器（数组实现）
    std::unordered_map<int, size_t> ref_; //哈希表：id→节点在堆中的索引
};

#endif
