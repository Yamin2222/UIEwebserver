#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <memory>

#include "epoller.h"
#include "heaptimer.h"
#include "sqlconnpool.h"
#include "threadpool.h"
#include "httpconn.h"

class WebServer {
public:
    WebServer(int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize);
    ~WebServer();
    void Start();

private:
    //初始化相关
    bool InitSocket_(); //初始化监听socket（创建、绑定、监听、设置非阻塞）
    void InitEventMode_(int trigMode); //初始化事件触发模式（ET/LT），设置listenEvent_和connEvent_
    void AddClient_(int fd, sockaddr_in addr); //添加新客户端连接（初始化 HttpConn、注册到 Epoller 等）
    
    //事件处理相关
    void DealListen_(); //处理监听socket的事件（新客户端连接请求）
    void DealWrite_(HttpConn* client); //处理客户端的可写事件（发送响应）
    void DealRead_(HttpConn* client); //处理客户端的可读事件（读取请求）

    //连接管理相关
    void SendError_(int fd, const char*info); //向客户端发送错误信息（如 404）
    void ExtentTime_(HttpConn* client); //延长客户端连接的超时时间（有活动时调用）
    void CloseConn_(HttpConn* client); //关闭客户端连接（从 Epoller、定时器中移除）

    //业务处理相关
    void OnRead_(HttpConn* client); //读取请求后的后续处理（解析请求）
    void OnWrite_(HttpConn* client); //发送响应后的后续处理（判断是否保持连接）
    void OnProcess(HttpConn* client); //处理请求的核心逻辑（生成响应）

    static const int MAX_FD = 65536; //最大文件描述符数量（受限于系统配置，通常为 65536）

    static int SetFdNonblock(int fd); //静态方法：设置文件描述符为非阻塞模式（被多个地方复用）

    int port_; //服务器端口
    bool openLinger_; //是否启用 SO_LINGER（优雅关闭连接）
    int timeoutMS_; //连接超时时间（毫秒）
    bool isClose_; //服务器是否关闭的标志
    int listenFd_; //监听socket的文件描述符
    char* srcDir_; //网页资源根目录（存放html、css等文件）
    
    uint32_t listenEvent_; //监听socket的事件类型（如 EPOLLIN | EPOLLET）
    uint32_t connEvent_;  //客户端连接的事件类型（如 EPOLLIN | EPOLLOUT | EPOLLET）
   
    std::unique_ptr<HeapTimer> timer_; //定时器（管理超时连接）
    std::unique_ptr<ThreadPool> threadpool_; //线程池（处理业务逻辑）
    std::unique_ptr<Epoller> epoller_; //IO 多路复用器（监视事件）
    std::unordered_map<int, HttpConn> users_; //// 客户端连接映射（fd到HttpConn对象的映射，快速查找）
};

#endif