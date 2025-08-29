#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h> //write
#include <sys/uio.h> //readv
#include <vector> //readv
#include <atomic>
#include <assert.h>

class Buffer {
public:
    Buffer(int initBufferSize  = 1024);
    ~Buffer() = default;

    size_t WritableBytes() const; //获取三个空间的大小
    size_t ReadableBytes() const;
    size_t PrependableBytes() const;

    const char* Peek() const; //readindex
    void EnsureWritable(size_t len);
    void HasWritten(size_t len);

    void Retrieve(size_t len); //读取函数
    void RetrieveUntil(const char* end);
    void RetrieveAll();
    std::string RetrieveAllToStr();

    const char* BeginWriteConst() const; //writeindex，只能得到位置
    char* BeginWrite(); //也是writeindex

    void Append(const std::string& str); //写入函数
    void Append(const char* str, size_t len); //C风格的字符串
    void Append(const void* data, size_t len); //任意类型的二进制数据
    void Append(const Buffer& buff); //把另一个缓冲区的对象加入到当前缓冲区

    ssize_t ReadFd(int fd, int* Errno); //从fd中读取数据到缓冲区
    ssize_t WriteFd(int fd, int* Errno); //将缓冲区数据写入到fd

private:
    char* BeginPtr_(); //Buffer头指针,获取缓冲区底层内存的起始地址
    const char* BeginPtr_() const; //Buffer头指针,用于只读访问缓冲区数据。
    void MakeSpace_(size_t len); //扩容函数

    std::vector<char> buffer_; //缓冲区
    //atomic是原子操作类型，用于实现多线程环境下的无锁同步
    //能保证对变量的读写操作是 “原子性” 的 —— 即操作不会被线程调度机制打断
    //要么完全执行，要么完全不执行
    //从而避免多线程并发访问时的数据竞争（Data Race）问题
    std::atomic<size_t> readIndex_; //读索引
    std::atomic<size_t> writeIndex_; //写索引

};

#endif