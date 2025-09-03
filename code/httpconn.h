/*
管理客户端的 socket 连接（文件描述符、地址信息）；
读取客户端发送的HTTP请求数据；
调用HttpRequest类解析请求；
调用HttpResponse类生成响应；
将响应数据发送给客户端。
*/

#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <sys/types.h>
#include <sys/uio.h>  //提供readv/writev函数（分散读写）
#include <arpa/inet.h> //提供sockaddr_in结构体（IPv4 地址）
#include <errno.h>

#include "log.h"
#include "buffer.h"
#include "httprequest.h"
#include "httpresponse.h"

//进行读写数据并调用httprequest 来解析数据以及httpresponse来生成响应

class HttpConn {
public:
    HttpConn();
    ~HttpConn();
    //sockaddr_in 是TCP/IP网络编程中专门用于描述“IPv4 地址和端口”的结构体
    //核心作用是给网络通信的两端贴个详细地址标签，让数据能准确找到要发往的目标
    void Init(int sockFd, const sockaddr_in& addr);

    ssize_t read(int* saveErrno);
    ssize_t write(int* saveErrno);

    void Close();
    int GetFd() const;
    int GetPort() const;
    const char* GetIP() const;
    sockaddr_in GetAddr() const;

    bool process(); //处理HTTP连接：解析请求并生成响应（核心函数）

    //返回待发送的字节数（用于判断是否还有数据未发送）
    size_t ToWriteBytes() {
        return static_cast<size_t>(iov_[0].iov_len) + static_cast<size_t>(iov_[1].iov_len);
    }

    bool IsKeepAlive() const {
        return request_.IsKeepAlive();
    }

    //边缘触发ET 还是水平触发LT
    //LT（水平触发）：只要缓冲区有数据未读，就会持续触发事件
    //ET（边缘触发）：仅在数据 “刚到达时” 触发一次事件
    static bool isET; //全局开关：是否启用边缘触发（ET）模式
    static const char* srcDir; //全局配置：静态资源的根目录（如"./www"）
    static std::atomic<int> userCount; //全局统计：当前连接的用户数（客户端数量）


private:
    int fd_; //客户端socket的文件描述符（唯一标识连接），一个fd就是一个客户端
    struct sockaddr_in addr_; //客户端的IP地址和端口信息

    bool isClose_;

    int iovCnt_; //分散读写的缓冲区数量（通常为 2）

    //用于writev函数的分散缓冲区
    struct iovec iov_[2]; ////iov_[0]指向响应头缓冲区，iov_[1]指向响应体

    Buffer readBuff_;
    Buffer writeBuff_;

    HttpRequest request_;
    HttpResponse response_;

};


#endif