#include "heaptimer.h"
using namespace std;

void HeapTimer::SwapNode_(size_t i, size_t j) {
    assert(i >=0 && i < heap_.size());
    assert(j >=0 && j < heap_.size());
    swap(heap_[i], heap_[j]);
    ref_[heap_[i].id] = i;
    ref_[heap_[j].id] = j;
}

void HeapTimer::siftup_(size_t i) {
    assert(i >= 0 && i < heap_.size());
    size_t parent = (i-1) / 2;
    while (i > 0) {
        //核心判断：如果父节点的超时时间>当前节点的超时时间（违反小根堆特性）
        if (heap_[parent] > heap_[i]) {
            SwapNode_(i, parent);
            i = parent;
            parent = (i-1) / 2;
        } else {
            break;
        }
    }
}

//小根堆的 “向下调整” 操作，用于当某个节点的值变大
//通常用于删除堆顶点后，更新某个节点的值使其变大，初始化堆顶时
bool HeapTimer::siftdown_(size_t i, size_t n) {
    assert(i >= 0 && i < heap_.size());
    assert(n >= 0 && n <=heap_.size());

    auto index = i; //当前节点索引
    auto child = 2*index+1; //做子节点索引
    while(child < n) {
        //选择左右子节点中较小的那个（小根堆特性）
        if(child+1 < n && heap_[child+1] < heap_[child]) {
            child++;
        }

        //如果子节点<当前节点（违反小根堆特性）
        if (heap_[child] < heap_[index]) {
            SwapNode_(index, child);  //交换当前节点与子节点
            index = child;            //更新当前节点为子节点位置
            child = 2 * child + 1;    //计算新的左子节点索引
        } else {
            //子节点>=当前节点：已满足小根堆特性，跳出循环
            break;
        }
    }
    //返回值：是否发生了位置调整（用于判断堆结构是否变化）
    return index > i;
}

// 删除指定位置的结点
void HeapTimer::del_(size_t index) {
    assert(index >= 0 && index < heap_.size());
    //记录要删除的索引，计算堆的最后一个节点索引
    size_t tmp = index;
    size_t n = heap_.size() - 1;
    assert(tmp <= n);
    //核心逻辑：如果要删除的节点不是最后一个节点，先交换到队尾
    if(index < heap_.size()-1) {
        SwapNode_(tmp, heap_.size()-1);
        if(!siftdown_(tmp, n)) {
            siftup_(tmp);
        }
    }
    //从哈希表中删除该节点的 id 映射
    ref_.erase(heap_.back().id);
    //从堆中删除最后一个节点
    heap_.pop_back();
}

//调整指定id对应的超时事件的超时时间
void HeapTimer::adjust(int id, int newExpires) {
    assert(!heap_.empty() && ref_.count(id));
    heap_[ref_[id]].expires = Clock::now() + MS(newExpires);
    //步骤2：向下调整堆，维持小根堆特性
    //为什么只调用 siftdown_ 而不调用 siftup_
    //新超时时间（当前时间 + newExpires）一定比原超时时间更晚
    siftdown_(ref_[id], heap_.size());
}

//向时间堆中添加或更新一个超时事件
void HeapTimer::add(int id, int timeOut, const TimeoutCallBack& cb) {
    assert(id >= 0);
    // 如果有，则调整
    if(ref_.count(id)) {
        int tmp = ref_[id];
        heap_[tmp].expires = Clock::now() + MS(timeOut);
        heap_[tmp].cb = cb;
        //关键：根据新值调整堆结构
        //先尝试向下调整，若未发生移动（返回false），则尝试向上调整
        if(!siftdown_(tmp, heap_.size())) {
            siftup_(tmp);
        }
    } else {
        //处理“新 id”（新增超时事件）
        size_t n = heap_.size(); //获取当前堆的大小
        ref_[id] = n; //在哈希表中记录 id 与索引的映射
        //向堆尾插入新节点：用初始化列表构造TimerNode（非默认构造，是直接初始化）
        heap_.push_back({id, Clock::now() + MS(timeOut), cb});
        //向上调整新节点
        siftup_(n);
    }
}

//当连接被主动关闭（如客户端断开、请求处理完成）时，需要手动触发其超时回调
void HeapTimer::doWork(int id) {
    if(heap_.empty() || ref_.count(id) == 0) {
        return;
    }
    size_t i = ref_[id];
    auto node = heap_[i];
    node.cb();  // 触发回调函数
    del_(i);
}

//批量处理所有已超时的节点
//这是时间堆的 “心跳函数”，定期调用以清理所有已超时的节点。
void HeapTimer::tick() {
    /* 清除超时结点 */
    if(heap_.empty()) {
        return;
    }
    while(!heap_.empty()) {
        TimerNode node = heap_.front();
        if(std::chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0) { 
            break; 
        }
        node.cb();
        pop();
    }
}

void HeapTimer::pop() {
    assert(!heap_.empty());
    del_(0);
}

void HeapTimer::clear() {
    ref_.clear();
    heap_.clear();
}

//获取下一次超时的剩余毫秒数
int HeapTimer::GetNextTick() {
    tick(); //先清理已经超时的连接
    int res = -1;
    if(!heap_.empty()) {
        //超时时间 - 当前时间 = 剩余时间
        res = static_cast<int>(std::chrono::duration_cast<MS>(heap_.front().expires - Clock::now()).count());
        if(res < 0) { res = 0; }
    }
    return res;
}