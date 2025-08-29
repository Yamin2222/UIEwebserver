#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H

#include <deque>
#include <condition_variable>
#include <mutex>
#include <sys/time.h>

//为什么新定义一个BlockQueue类而不是使用std::deque？
//因为std::deque没有线程安全的push和pop操作，
//而BlockQueue类添加线程安全控制和阻塞机制，通过互斥锁和条件变量，实现线程安全
template<typename T>
class BlockQueue {
public:
    explicit BlockQueue(size_t maxsize = 1000);
    ~BlockQueue();

    bool empty();
    bool full();
    void push_back(const T& item);
    void push_front(const T& item);
    bool pop(T& item);
    bool pop(T& item, int timeout); //等待时间
    void clear();
    T front();
    T back();
    size_t capacity();
    size_t size();

    //唤醒所有消费线程，强制“消费”队列里所有数据
    void flush();
    //关闭队列，终止所有阻塞等待的线程，释放资源
    void Close();

private:
    std::deque<T> deq_;
    //互斥锁，用于保护deque的线程安全，保证同一时间只有一个线程能访问共享资源
    std::mutex mtx_;
    //关闭标志，标记队列是否已关闭
    bool isClose_;
    size_t capacity_; //队列最大容量
    //消费者条件变量，于让消费者线程（取数据的线程）阻塞等待
    std::condition_variable condConsumer_;
    //生产者条件变量，用于让生产者线程（放数据的线程）阻塞等待
    std::condition_variable condProducer_;
};

template<typename T>
BlockQueue<T>::BlockQueue(size_t maxsize) : capacity_(maxsize) {
    assert(maxsize > 0);
    isClose_ = false;
}

template<typename T>
BlockQueue<T>::~BlockQueue() {
    Close();
}

//清除数据-标记关闭-唤醒所有阻塞线程
template<typename T>
void BlockQueue<T>::Close() {
    clear();
    isClose_ = true;
    condProducer_.notify_all();
    condConsumer_.notify_all();
}

template<typename T>
void BlockQueue<T>::clear() {
    //创建了一个std::lock_guard对象lock，并传入互斥锁mtx_
    //在lock对象的作用域结束时，互斥锁会自动释放
    std::lock_guard<std::mutex> lock(mtx_);
    deq_.clear();
}

template<typename T>
bool BlockQueue<T>::empty() {
    //同理
    std::lock_guard<std::mutex> lock(mtx_);
    return deq_.empty();
}

template<typename T>
bool BlockQueue<T>::full() {
    std::lock_guard<std::mutex> lock(mtx_);
    return deq_.size() >= capacity_;
}

template<typename T>
void BlockQueue<T>::push_back(const T& item) {
    //unique_lock支持手动解锁和加锁
    //后续用到了wait，wait期间需要解锁，后续唤醒时重新锁定
    std::unique_lock<std::mutex> locker(mtx_);
    while (deq_.size() >= capacity_) {
        condProducer_.wait(locker);
    }
    deq_.push_back(item);
    condConsumer_.notify_one();
}

template<typename T>
void BlockQueue<T>::push_front(const T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while (deq_.size() >= capacity_) {
        condProducer_.wait(locker); 
    }
    deq_.push_front(item);
    condConsumer_.notify_one();
}

template<typename T>
bool BlockQueue<T>::pop(T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()) {
        condConsumer_.wait(locker);
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

template<typename T>
bool BlockQueue<T>::pop(T& item, int timeout) {
    std::unique_lock<std::mutex> locker(mtx_);
    while (deq_.empty()) {
        if (condConsumer_.wait_for(locker, std::chrono::seconds(timeout))
                                    == std::cv_status::timeout) {
            return false; // 超时
        }
        if (isClose_) return false;
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

template<typename T>
T BlockQueue<T>::front() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.front();
}

template<typename T>
T BlockQueue<T>::back() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.back();
}

template<typename T>
size_t BlockQueue<T>::capacity() {
    std::lock_guard<std::mutex> locker(mtx_);
    return capacity_;
}

template<typename T>
size_t BlockQueue<T>::size() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size();
}

template<typename T>
void BlockQueue<T>::flush() {
    condConsumer_.notify_one();
}

#endif // BLOCKQUEUE_H