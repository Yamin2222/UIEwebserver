#include "buffer.h"
#include <algorithm>
#include <errno.h>

//vector<char>初始化
Buffer::Buffer(int initBufferSize): buffer_(initBufferSize), readIndex_(0), writeIndex_(0) {}

//可写的数量（长度）
size_t Buffer::WritableBytes() const {
    return buffer_.size() - writeIndex_;
}
//可读的数量（长度）
size_t Buffer::ReadableBytes() const {
    return writeIndex_ - readIndex_;
}
//可预留的空间（长度）
size_t Buffer::PrependableBytes() const {
    return readIndex_;
}


const char* Buffer::Peek() const {
    return &buffer_[readIndex_];
}

void Buffer::EnsureWritable(size_t len) {
    if (Buffer::WritableBytes() < len) {
        MakeSpace_(len);
    }
    assert(len <= WritableBytes()); //再次验证，防御性编程
}

//写入len长度，修改writeIndex下标
void Buffer::HasWritten(size_t len) {
    writeIndex_ += len;
}

//读取len长度，修改readIndex下标
void Buffer::Retrieve(size_t len) {
    if (len < ReadableBytes()) {
        readIndex_ += len;
    } else {
        RetrieveAll();
    }
}

//读取数据到end为止
void Buffer::RetrieveUntil(const char* end) {
    assert(Peek() <= end); //end必须位于readIndex后
    Retrieve(end - Peek());
}

void Buffer::RetrieveAll() {
    //bzero 是 C 语言中的函数，作用是将一块内存的所有字节清零
    //但是比较耗时实际上，只重置指针就能 “逻辑上清空数据”
    //后续写入会覆盖旧数据，无需真正清零内存
    // bzero(&buffer_[0], buffer_.size());
    // 重置读写指针到起始位置，避免保留前置空洞
    readIndex_ = 0;
    writeIndex_ = 0;
}

std::string Buffer::RetrieveAllToStr() {
    std::string str(Peek(), ReadableBytes());
    RetrieveAll();
    return str;
}

const char* Buffer::BeginWriteConst() const {
    return &buffer_[writeIndex_];
}

char* Buffer::BeginWrite() {
    return &buffer_[writeIndex_];
}

void Buffer::Append(const char* str, size_t len) {
    assert(str);
    EnsureWritable(len);
    //将str复制到write空间，注意copy的使用方法
    std::copy(str,str+len,BeginWrite()); 
    HasWritten(len);
}
void Buffer::Append(const std::string& str) {
    Append(str.c_str(), str.size());
}
void Buffer::Append(const void* data, size_t len) {
    Append(static_cast<const char*>(data), len);
}
void Buffer::Append(const Buffer& buff) {
    Append(buff.Peek(), buff.ReadableBytes());
}

//接下来用到的write和readv都是linux系统的系统调用，在<unistd.h>和<sys/uio.h>头文件中 

//从文件描述符读取数据
ssize_t Buffer::ReadFd(int fd, int* Errno) {
    //栈上临时缓冲区，
    char buff[65545];
    //分散读（scatter-gather I/O）的缓冲区结构
    //iovec结构体包含两个成员：iov_base（缓冲区基地址）和iov_len（缓冲区长度）
    //第一个是Buffer自身的区域
    //第二个是buff临时缓冲区
    struct iovec iov[2];
    size_t writeable = WritableBytes();
    iov[0].iov_base = BeginWrite();
    iov[0].iov_len = writeable;
    iov[1].iov_base = buff;
    iov[1].iov_len = sizeof(buff);

    //readv 是 Linux 提供的 “分散读” 系统调用
    //功能是：从 fd 读取数据，先填满 iov[0]（直到 iov[0].iov_len）
    //剩余数据再填满 iov[1]（直到 iov[1].iov_len）
    //返回值 len 是实际读取的总字节数（若失败则为 -1）
    ssize_t len = readv(fd, iov, 2);
    if (len < 0) {
        //将系统调用失败的错误码（errno）保存到外部传入的Errno指针指向的变量中
        *Errno = errno; //errno 是C/C++标准库中定义的全局错误码变量
    } else if (static_cast<size_t>(len) <= writeable) {
        //数据完全放入 Buffer 的情况
        writeIndex_ += len;
    } else {
        //数据部分放入临时缓冲区的情况
        writeIndex_ = buffer_.size();
        //将临时缓冲区中剩余的数据追加到Buffer中
        Append(buff, static_cast<size_t>(len-writeable));
    }
    return len;
}

//向文件描述符（通常是网络 socket）写入数据
ssize_t Buffer::WriteFd(int fd, int* Errno) {
    //write 是系统调用，功能是将数据从用户空间写入内核空间
    //fd：目标文件描述符（如 socket）。
    //Peek()：返回缓冲区可读数据的起始地址（const char*），即要发送的数据的起始位置。
    //ReadableBytes()：返回可读数据的长度（size_t），即要发送的数据总字节数
    //返回值 len：实际写入的字节数
    ssize_t len = write(fd, Peek(), ReadableBytes());
    if (len < 0) {
        *Errno = errno;
        return len;
    }
    Retrieve(len);
    return len;
}

char* Buffer::BeginPtr_() {
    return &buffer_[0];
}

const char* Buffer::BeginPtr_() const{
    return &buffer_[0];
}

//空间扩容
void Buffer::MakeSpace_(size_t len) {
    if (WritableBytes() + PrependableBytes() < len) {
        //加上前置空间也不够时，直接扩容
        //buffer底层是vector，所以直接resize，额外 +1 通常是预留一个字节，避免边界问题
        buffer_.resize(writeIndex_ + len + 1);
    } else {
        //空间充足时，移动数据回收前置区域
        size_t readable = ReadableBytes(); //有效数据的长度
        //将有效数据从[readIndex_, writeIndex_)移动到缓冲区起始位置[0, readable)
        std::copy(BeginPtr_()+readIndex_, BeginPtr_()+writeIndex_,BeginPtr_());
        //重置读写指针
        readIndex_ = 0;
        writeIndex_ = readable;
        assert(readable == ReadableBytes());
    }
}


