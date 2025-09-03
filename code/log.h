#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/stat.h>
#include "blockqueue.h"
#include "buffer.h"

class Log {
public:
    void init(int level, const char* path = "./log",
            const char* suffix = ".log",
            int maxQueueSize = 0); //初始化日志，不在构造函数中初始化，是参数更灵活
    
    static Log* Instance(); //单例模式，构造函数在私有区，通过Instance()获取唯一实例
    static void FlushLogThread(); //异步写日志公有方法

    //写入，其中format是格式化字符串，就像printf的格式化字符串
    //使用时，可以这样：write(info,"abcd %d", 123);
    void write(int level, const char* format, ...);
    void flush(); //刷新日志

    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() { return isOpen_; }

private:
    Log();
    void AppendLogLevelTitle_(int level); //添加日志级别标题
    virtual ~Log();
    void AsyncWrite_(); //异步写日志

private:
    static const int LOG_PATH_LEN = 256; //日志路径长度
    static const int LOG_NAME_LEN = 256; //日志名称长度
    static const int MAX_LINES =50000; //日志文件内的最长行数

    const char* path_; //日志路径
    const char* suffix_; //日志后缀

    int MAX_LINES_; //最大日志行数

    int lineCount_; //日志行数记录
    int toDay_; //按当天日期区分文件

    bool isOpen_;

    Buffer buff_; //输出内容，缓冲区
    int level_; //日志等级
    bool isAsync_; //是否异步日志
    
    //FILE是一个标准库的结构体，用于保存文件的相关信息
    //FILE*类型的指针是操作文件的句柄，所有操作函数都要通过它来操作
    FILE* fp_; //文件指针
    std::unique_ptr<BlockQueue<std::string>> deque_; //阻塞队列
    std::unique_ptr<std::thread> writeThread_; //写线程的指针
    std::mutex mtx_; //同步日志用的互斥锁
};

#define LOG_BASE(level, format, ...) \
    do { \
        Log* log = Log::Instance();\
        if (log->IsOpen() && log->GetLevel() <= level) { \
            log->write(level, format, ##__VA_ARGS__); \
            log->flush(); \
        }\
    }while(0);

// 四个宏定义，主要用于不同类型的日志输出，也是外部使用日志的接口
// ...表示可变参数，__VA_ARGS__就是将...的值复制到这里
// 前面加上##的作用是：当可变参数的个数为0时，这里的##可以把把前面多余的","去掉,否则会编译出错。
#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while(0);    
#define LOG_INFO(format, ...) do {LOG_BASE(1, format, ##__VA_ARGS__)} while(0);
#define LOG_WARN(format, ...) do {LOG_BASE(2, format, ##__VA_ARGS__)} while(0);
#define LOG_ERROR(format, ...) do {LOG_BASE(3, format, ##__VA_ARGS__)} while(0);

#endif //LOG_H