#include "webserver.h"
#include <limits.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>

using namespace std;

WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
    {
    // 确定资源目录（优先使用编译期指定的 RESOURCE_DIR；其次根据运行目录智能回退）
    {
        // 1. 获取当前工作目录（服务器启动时的目录）
        char cwdBuf[PATH_MAX] = {0};
        char* cwd = getcwd(cwdBuf, sizeof(cwdBuf));
        if (!cwd) {  // 替换 assert，避免程序直接崩溃，改为记录错误并标记服务器关闭
            LOG_ERROR("Failed to get current working directory! errno: %d", errno);
            isClose_ = true;
            srcDir_ = nullptr;
            return;
        }
        std::string cwdStr(cwd);

        // 2. 智能拼接资源目录（支持从 bin 目录启动的场景）
        std::string resDir;
        if (cwdStr.size() >= 4 && cwdStr.substr(cwdStr.size() - 4) == "/bin") {
            // 如果从 bin 目录启动，资源目录为上级目录的 resources
            resDir = cwdStr.substr(0, cwdStr.size() - 4) + "/resources/";
        } else {
            // 否则直接在当前目录下找 resources
            resDir = cwdStr + "/resources/";
        }

        // 3. 路径规范化（消除 .. 等符号，更清晰）
        char normalizedPath[PATH_MAX] = {0};
        if (realpath(resDir.c_str(), normalizedPath) == nullptr) {
            LOG_WARN("Resource path may not exist: %s (errno: %d), will use as-is", resDir.c_str(), errno);
            strncpy(normalizedPath, resDir.c_str(), PATH_MAX - 1);  // 用原始路径兜底
        }

        // 4. 安全分配内存并存储路径
        srcDir_ = static_cast<char*>(malloc(strlen(normalizedPath) + 1));
        if (!srcDir_) {  // 内存分配失败处理
            LOG_ERROR("Failed to allocate memory for resource directory!");
            isClose_ = true;
            return;
        }
        strcpy(srcDir_, normalizedPath);
    }
    HttpConn::userCount = 0; //初始化客户端连接计数
    HttpConn::srcDir = srcDir_; //给HttpConn类设置资源目录

    //初始化数据库连接池（单例模式）
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    InitEventMode_(trigMode); //初始化事件触发模式（ET/LT）
    if(!InitSocket_())  {
        isClose_ = true; //初始化监听socket
    }
    //初始化日志系统
    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

//析构函数
WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

//初始化事件触发模式（ET/LT）
void WebServer::InitEventMode_(int trigMode) {
    //基础事件初始化
    //listenEvent_负责接收新连接，connEvent_负责与客户端收发数据
    listenEvent_ = EPOLLRDHUP; //监听 socket 的基础事件：检测到连接异常关闭
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP; //客户端连接的基础事件
    switch (trigMode)
    {
    case 0: //LT / LT
        break;
    case 1: //LT / ET
        connEvent_ |= EPOLLET;
        break;
    case 2: //ET / LT
        listenEvent_ |= EPOLLET;
        break;
    case 3: //ET / ET
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {
    int timeMS = -1; //超时时间变量（传给 epoll_wait）

    if(!isClose_) LOG_INFO("========== Server start ==========");
    
    //服务器主循环
    while(!isClose_) {
        //步骤1：获取下一次超时的时间（由定时器决定）
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick(); //从堆定时器中获取最近的超时时间
        }

        //步骤2：等待事件发生（阻塞在这里，直到有事件或超时）
        int eventCnt = epoller_->Wait(timeMS);

        // 新增：处理 epoll_wait 错误
        if (eventCnt < 0) {
            LOG_ERROR("epoll_wait failed! errno: %d", errno);
            // 若错误是致命的（如 EINVAL，epoll_fd 无效），应退出进程
            if (errno != EINTR) {  // EINTR 是被信号中断，可忽略
                isClose_ = true;
                break;
            }
        }

        //步骤3：遍历所有就绪事件，分发给对应逻辑处理
        for(int i = 0; i < eventCnt; i++) {
            int fd = epoller_->GetEventFd(i); //获取触发事件的 fd
            uint32_t events = epoller_->GetEvents(i); //获取事件类型
            //分支1：如果是监听socket的事件（新客户端连接）
            if(fd == listenFd_) {
                DealListen_();
            }
            //分支2：如果是连接关闭/错误事件（客户端断开或出错）
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                assert(users_.count(fd) > 0); //确保该fd存在于客户端映射中
                CloseConn_(&users_[fd]);
            }
            //分支3：如果是可读事件（客户端发来了数据）
            else if(events & EPOLLIN) {
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            //分支4：如果是可写事件（可以给客户端发数据了）
            else if(events & EPOLLOUT) {
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            } else {
                //未知事件，记录错误日志
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info) {
    assert(fd > 0);
    //向客户端 fd 发送错误信息
    //send是系统调用，通过客户端的socket描述符fd，把错误信息info发送给客户端
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn* client) {
    assert(client);
    int fd = client->GetFd();
    LOG_INFO("Client[%d] quit!", fd);
    epoller_->DelFd(fd);
    client->Close();
    // 连接关闭后，从映射中移除，避免长期膨胀
    users_.erase(fd);
}

void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    //初始化该 fd 对应的 HttpConn 对象
    users_[fd].Init(fd, addr);
    //如果设置了超时时间，给这个客户端添加定时器（仅捕获fd，避免悬垂指针）
    if(timeoutMS_ > 0) {
        int cfd = fd;
        timer_->add(cfd, timeoutMS_, [this, cfd]() {
            auto it = users_.find(cfd);
            if (it != users_.end()) {
                CloseConn_(&it->second);
            } else {
                epoller_->DelFd(cfd);
            }
        });
    }
    // 先设置非阻塞，再注册到 Epoller（避免短暂阻塞风险）
    SetFdNonblock(fd);
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

//处理监听套接字，主要逻辑是accept新的套接字，并加入timer和epoller中
void WebServer::DealListen_() {
    //定义客户端地址结构体（用于存储新连接的客户端 IP 和端口）
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    //循环接收新连接（是否循环取决于监听socket的触发模式）
    do {
        //调用accept()接收新连接，获取客户端socket的fd和地址信息
        //accept()为系统调用，
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        if(fd <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("accept error! errno: %d", errno);
            }
            return;
        }
        //处理服务器连接数满的情况
        else if(HttpConn::userCount >= MAX_FD) {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);
    } while(listenEvent_ & EPOLLET);
}

//这两是处理“客户端读写事件”的调度函数
void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client); //延长该客户端的超时时间（有活动，说明没闲置）
    //将“读事件的实际处理逻辑”封装 成任务，交给线程池执行
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client)); //这是一个右值，bind将参数和函数绑定
}

void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

//延长客户端连接的超时时间
void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    //调用定时器的adjust方法，把它的超时时间重新设置为 timeoutMS_ 毫秒
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}

void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1; //取的字节数
    int readErrno = 0; //错误码（用于区分正常和异常情况）

    //调用 HttpConn 的 read 方法，从客户端 fd 读取数据到读缓冲区
    ret = client->read(&readErrno);   

    if(ret <= 0 && readErrno != EAGAIN) {
        //情况1：读取失败且不是“暂时无数据”（真正的错误）
        CloseConn_(client);
        return;
    }
    //情况2：读取成功（或暂时无数据但连接正常），进入请求处理阶段
    OnProcess(client);
}

void WebServer::OnProcess(HttpConn* client) {
    if(client->process()) { 
        //情况1：请求解析完成（且生成了响应），需要切换到“监听可写事件”
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);  
    } else {
        //情况2：请求未解析完成（需要更多数据），继续监听“可读事件”
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1; //发送的字节数
    int writeErrno = 0; //错误码（区分正常和异常情况）

    //调用 HttpConn 的 write 方法，将写缓冲区的数据发送给客户端
    ret = client->write(&writeErrno);

    //情况1：所有数据都已发送完成（写缓冲区为空）
    if(client->ToWriteBytes() == 0) {
        //如果是长连接（Connection: keep-alive）
        if(client->IsKeepAlive()) {
            //调整Epoll 监控事件为“可读”，等待客户端的下一次请求
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
            return;
        }
    }
    //情况2：数据未发完，但错误是“暂时无法发送”（EAGAIN）
    else if(ret < 0) {
        if(writeErrno == EAGAIN) {
            //调整 Epoll 继续监控“可写事件”，等待下次能发送时再试
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    //其他情况（数据发送失败/短连接）：关闭连接
    CloseConn_(client);
}

//创建socket的核心函数
bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;
    //端口合法性检查
    //0-1023 是 “知名端口”（如 80 是 HTTP），通常使用 1024 以上的端口
    if(port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    //初始化服务器地址结构体
    addr.sin_family = AF_INET; //使用 IPv4 协议
    addr.sin_addr.s_addr = htonl(INADDR_ANY); //监听所有网卡的IP（服务器可能有多个网卡）
    addr.sin_port = htons(port_); //绑定到指定端口（注意：需转换为网络字节序）

    {
    //配置socket优雅关闭（SO_LINGER 选项）
    struct linger optLinger = { 0 };
    if(openLinger_) { //如果开启了优雅关闭（构造函数传入的 OptLinger 参数）
        optLinger.l_onoff = 1; //启用 SO_LINGER 选项
        optLinger.l_linger = 1; //延迟关闭时间（1 秒）
    }

    //创建监听socket（TCP 类型）
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!");
        return false;
    }
    //设置 SO_LINGER 选项
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!");
        return false;
    }
    }

    //允许端口复用（SO_REUSEADDR选项）
    //SO_REUSEADDR作用：允许服务器重启时立即复用同一端口
    int optval = 1;
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    //绑定端口（bind）
    //将创建的listenFd_与前面配置的地址结构体（IP+端口）绑定，使socket与特定端口关联
    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    //开始监听
    ret = listen(listenFd_, 6); //第二个参数是“连接请求队列”的最大长度
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    //将监听socket注册到epoll
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN);
    if(!ret) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }

    //设置监听socket为非阻塞模式
    SetFdNonblock(listenFd_);   
    LOG_INFO("Server port:%d", port_);
    return true;
}

int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}