//管理服务器与单个客户端之间的 “TCP 连接 + HTTP 通信” 全生命周期
#include "httpconn.h"
#include <errno.h>
using namespace std;

const char* HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() {
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;
}

HttpConn::~HttpConn() {
    Close();
}

void HttpConn::Init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    userCount++; //原子递增在线用户数（统计当前连接数）
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();
    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close() {
    response_.UnmapFile(); //释放响应中通过内存映射的文件资源
    if (isClose_ == false) {
        isClose_ = true;
        userCount--;
        close(fd_); //close为系统调用函数，关闭客户端socket的文件描述符，释放TCP连接
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const {
    return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const {
    return addr_;
}

const char* HttpConn::GetIP() const {
    //addr_.sin_addr存储的是32位整数形式的 IP 地址
    //ntoa = network to ASCII是系统函数，将整数形式的IP转换为字符串形式
    return inet_ntoa(addr_.sin_addr); //返回客户端的IP地址
}

int HttpConn::GetPort() const {
    return ntohs(addr_.sin_port); //返回客户端的端口号
}

//从客户端读取数据到缓冲区
ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1; //存储单次/总读取的字节数，初始化为-1（表示未成功读取）
    //循环读取数据（是否循环取决于触发模式）
    do {
        //调用缓冲区的 ReadFd 方法，从 fd_ 读取数据到 readBuff_
        //参数：fd_ 是客户端 socket 描述符，saveErrno 用于保存错误码
        len = readBuff_.ReadFd(fd_, saveErrno);

        //如果读取的字节数 <= 0（读取失败或无数据），跳出循环
        if (len <= 0) {
            break;
        }
    } while (isET); //循环条件：如果是ET模式，则继续读取直到无数据
    return len; //返回读取的总字节数
}

ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1; //存储单次/总读取的字节数，初始化为-1（表示未成功读取）
    do {
        //调用writev系统调用，将iov_中的两个缓冲区数据发送到fd_
        len = writev(fd_, iov_, iovCnt_);
        
        if (len <= 0) {
            //发送失败：保存错误码到saveErrno，跳出循环
            *saveErrno = errno;
            break; 
        }
        //检查是否所有数据都已发送完毕
        if (iov_[0].iov_len + iov_[1].iov_len  == 0) {
            break; //传输结束
        } else if (static_cast<size_t>(len) > iov_[0].iov_len) {
            //情况1：已发送的数据超过第一个缓冲区（iov_[0]）的长度
            //调整第二个缓冲区（iov_[1]）的指针和长度：减去已发送的部分
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            //如果第一个缓冲区有数据，清空并重置
            if(iov_[0].iov_len) {
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        } else {
            //情况2：已发送的数据未超过第一个缓冲区的长度
            //调整第一个缓冲区的指针和长度：减去已发送的部分
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; 
            iov_[0].iov_len -= len; 
            writeBuff_.Retrieve(len); //从写缓冲区中移除已发送的数据
        }
    } while (isET || ToWriteBytes() > 10240); //循环条件：ET模式或剩余数据量较大
    return len;
}

bool HttpConn::process() {
    //步骤1：初始化请求对象
    request_.Init();

    //步骤2：检查读缓冲区是否有数据（没有数据则无法处理）
    if (readBuff_.ReadableBytes() <= 0) {
        return false;
    } else if (request_.parse(readBuff_)) {
        //步骤3：解析读缓冲区中的HTTP请求
        LOG_DEBUG("%s", request_.path().c_str());
        //初始化响应：200表示成功，根据请求决定是否保持连接
        response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    } else {
        //解析失败：返回400错误（Bad Request），且不保持连接
        response_.Init(srcDir, request_.path(), false, 400);
    }
    //步骤4：生成响应报文，写入写缓冲区（响应头+部分响应体）
    response_.MakeResponse(writeBuff_);

    //步骤5：绑定响应头到iov_[0]（准备发送）
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek()); //指向响应头数据
    iov_[0].iov_len = writeBuff_.ReadableBytes(); //响应头长度
    iovCnt_ = 1;

    //步骤6：如果有响应体（文件），绑定到iov_[1]
    if(response_.FileLen() > 0  && response_.File()) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }
    //打印调试日志：文件大小、缓冲区数量、总待发送字节数
    LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen() , iovCnt_, ToWriteBytes());
    return true;
}

