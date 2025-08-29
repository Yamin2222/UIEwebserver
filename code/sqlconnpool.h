#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include "log.h"

class SqlConnPool {
public:
    static SqlConnPool* Instance();

    MYSQL* GetConn(); //获取连接
    void FreeConn(MYSQL* conn); //释放连接
    int GetFreeConnCount(); //获取空闲连接数

    void Init(const char* host, int port,
              const char* user, const char* pwd,
              const char* dbName, int connSize); //初始化

    void ClosePool();

private:
    SqlConnPool() = default;
    ~SqlConnPool() {
        ClosePool();
    }
    int MAX_CONN_; //最大连接数
    std::queue<MYSQL*> connQue_; //连接队列
    std::mutex mtx_; //互斥锁
    sem_t semId_; //信号量
};

//RAII机制，自动释放资源
class SqlConnRAII {
public:
    SqlConnRAII(MYSQL** sql, SqlConnPool* connpool) {
        assert(connpool);
        *sql = connpool->GetConn();
        sql_ = *sql;
        connpool_ = connpool;
    }

    ~SqlConnRAII() {
        if (sql_) {
            connpool_->FreeConn(sql_);
        }
    }
private:
    MYSQL* sql_;
    SqlConnPool* connpool_;
};
#endif