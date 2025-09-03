#include "epoller.h"

Epoller::Epoller(int maxEvent) : epollFd_(epoll_create1(0)), events_(maxEvent) { 
    // 改用 epoll_create1(0) 替代 epoll_create(512)（现代推荐用法）
    assert(epollFd_ >= 0 && events_.size() > 0);
}

Epoller::~Epoller() {
    close(epollFd_);
}

bool Epoller::AddFd(int fd, uint32_t events) {
    if (fd < 0) return false;
    //创建并初始化epoll_event结构体
    epoll_event ev = {0};
    ev.data.fd = fd; //填写“监控目标”：告诉epoll要监视的是哪个fd
    ev.events = events; //填写“关注的事件”：告诉epoll要监视这个fd的哪些状态
    //其中epollFd是epoll实例，用于监控
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
}

bool Epoller::ModFd(int fd, uint32_t events) {
    if (fd < 0) return false;
    epoll_event ev = {0};
    ev.data.fd = fd;
    ev.events = events;
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev); // 调用 EPOLL_CTL_MOD 修改事件
}

bool Epoller::DelFd(int fd) {
    if(fd < 0) return false;
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, 0);
}

//等待并收集就绪事件，等待时间为timeoutMs
//返回的是就绪事件的数量
int Epoller::Wait(int timeoutMs) {
    // 调用系统函数 epoll_wait，等待事件发生
    return epoll_wait(
        epollFd_,          // 1. 监控中心标识（哪个 epoll 实例）
        &events_[0],       // 2. 输出参数：存放“触发事件的设备列表”的缓冲区
        static_cast<int>(events_.size()),  // 3. 缓冲区最大能存放的事件数量
        timeoutMs          // 4. 超时时间（毫秒，-1 表示无限等待）
    );
}

//获取事件fd
int Epoller::GetEventFd(size_t i) const {
    assert(i < events_.size()); 
    return events_[i].data.fd;
}

//获取事件属性
uint32_t Epoller::GetEvents(size_t i) const {
    assert(i < events_.size()); 
    return events_[i].events;
}