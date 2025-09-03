#include "sqlconnpool.h"

//单例模式，静态局部变量
SqlConnPool* SqlConnPool::Instance() {
    static SqlConnPool Pool;
    return &Pool;
}

void SqlConnPool::Init(const char* host, int port,
                        const char* user, const char* pwd,
                        const char* dbName, int connSize) {
    assert(connSize > 0); //断言，connSize必须大于0
    for (int i = 0; i < connSize; ++i) {
        MYSQL* conn = nullptr;
        conn = mysql_init(conn); //初始化一个连接
        if (!conn) {
            LOG_ERROR("MySql init error!");
            assert(conn);
        }
        conn = mysql_real_connect(conn, host, user, pwd, dbName, port, nullptr,0);
        if (!conn) {
            LOG_ERROR("MySql Connect error!");
        }
        connQue_.emplace(conn);
    }
    MAX_CONN_ = connSize;
    sem_init(&semId_, 0, MAX_CONN_); //初始化信号量
}

MYSQL* SqlConnPool::GetConn() {
    MYSQL* conn = nullptr;
    // 阻塞等待空闲连接，避免返回 nullptr 触发上层断言
    sem_wait(&semId_);
    std::lock_guard<std::mutex> locker(mtx_); //互斥锁是等待其他线程释放资源
    if (!connQue_.empty()) {
        conn = connQue_.front(); //取出队首连接
        connQue_.pop();
    }
    return conn;
}

//free操作只释放连接，不关闭连接池
void SqlConnPool::FreeConn(MYSQL* conn) {
    assert(conn);
    std::lock_guard<std::mutex> locker(mtx_); //mutex保护的是队列
    connQue_.push(conn);
    sem_post(&semId_);
}

void SqlConnPool::ClosePool() {
    std::lock_guard<std::mutex> locker(mtx_);
    while(!connQue_.empty()) {
        MYSQL* conn = connQue_.front();
        connQue_.pop();
        mysql_close(conn);
    }
    mysql_library_end();
}

int SqlConnPool::GetFreeConnCount() {
    std::lock_guard<std::mutex> locker(mtx_);
    return connQue_.size();
}