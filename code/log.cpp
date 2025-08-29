#include "log.h"

Log::Log() {
    fp_ = nullptr;
    deque_ = nullptr;
    writeThread_ = nullptr;
    lineCount_ = 0;
    toDay_ = 0;
    isOpen_ = false;
    isAsync_ = false;
}

Log::~Log() {
    // 优先安全关闭异步资源
    if (isAsync_ && deque_) {
        while (!deque_->empty()) {
            deque_->flush(); // 唤醒所有阻塞线程
        }
        deque_->Close(); // 关闭阻塞队列
        if (writeThread_) {
            // join 之前确保线程存在且可 join
            if (writeThread_->joinable()) {
                writeThread_->join();
            }
        }
    }
    // 关闭文件句柄
    if (fp_) {
        std::lock_guard<std::mutex> locker(mtx_);
        flush();
        fclose(fp_);
        fp_ = nullptr;
    }
}

//刷新，唤醒阻塞队列消费者
//用内存缓冲减少磁盘 I/O 次数
//日志系统为了提高性能，先将日志数据暂存在内存缓冲区
//缓冲区满足一定条件时，通过flush操作将缓冲区的数据一次性写入磁盘。
void Log::flush() {
    //异步模式下日志的生成和是由业务线程完成
    //写入则是独立的写线程
    if (isAsync_) {
        if (deque_) deque_->flush();
    }
    //同步模式下，日志的生成和写入时同一个线程完成
    if (fp_) fflush(fp_);
}

Log* Log::Instance() {
    static Log log; //局部静态变量，线程安全
    return &log;
}

//线程入口函数，仅负责 “启动异步写入的核心逻辑”，不包含具体的写入代码。
//创建线程时需要传入一个可调用对象，AsyncWrite_是类的非静态成员函数
//无法直接作为线程入口函数
//因此需要通过类的实例对象来调用它（需要this指针）
void Log::FlushLogThread() {
    Log::Instance()->AsyncWrite_();
}

void Log::AsyncWrite_() {
    std::string str = "";
    while (deque_->pop(str)) { //从阻塞队列中取出一条日志
        std::lock_guard<std::mutex> locker(mtx_);
        fputs(str.c_str(), fp_); //写入文件
    }
}

//初始化函数
void Log::init(int level, const char* path,
        const char* suffix, int maxQueueSize) {
    isOpen_ = true;
    level_ = level;
    path_ = path;
    suffix_ = suffix;
    //maxQueueSize > 0表示异步
    if (maxQueueSize > 0) {
        isAsync_ = true;
        if (!deque_) {
            deque_.reset(new BlockQueue<std::string>(maxQueueSize));
            writeThread_.reset(new std::thread(FlushLogThread));
        }
    } else {
        //同步日志则将isAsync_设置为flase
        isAsync_ = false;
    }

    lineCount_ = 0; //当前日志行数
    //用ullptr获取当前系统时间
    time_t timer = time(nullptr);
    //通过localtime获取本地时区的时间
    struct tm* systime = localtime(&timer);
    char fileName[LOG_NAME_LEN] = {0};
    //snprintf用于获取文件名
    //顺序依次是：路径-年（需要加1900）-月（需要加1）-日-后缀
    snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s",
            path_, systime->tm_year + 1900, systime->tm_mon + 1,
            systime->tm_mday, suffix_);
    toDay_ = systime->tm_mday;
    //用括号来限定lock_guard的作用范围
    {
        std::lock_guard<std::mutex> locker(mtx_);
        buff_.RetrieveAll();
        //如果文件存在，那先刷新并关闭
        if (fp_) {
            flush();
            fclose(fp_);
        }
        fp_ = fopen(fileName, "a");
        if (fp_ == nullptr) {
            mkdir(path_, 0777);
            fp_ = fopen(fileName, "a");
        }
        assert(fp_ != nullptr);
    }
}

void Log::write(int level, const char* format, ...) {
    //timerval结构体包含两个参数
    //1：time_t tv_sec，秒数时间戳，相当于time(nulptr)
    //2：suseconds_t tv_usec,微秒
    //精度高于time_t timer = time(nullptr)
    struct  timeval now = {0,0};
    //系统调用函数，获取当前时间并写入now
    gettimeofday(&now, nullptr);
    time_t tSec = now.tv_sec;
    struct tm *sysTime = localtime(&tSec);
    struct tm t = *sysTime;

    //用于处理可变参数
    //使用步骤：定义-初始化-解析可变参数-结束解析
    va_list vaList;

    //如果日志日期不是今天，或者日志行数超了
    if (toDay_ != t.tm_mday || (lineCount_ &&(lineCount_ && (lineCount_  %  MAX_LINES == 0)))) {
        std::unique_lock<std::mutex> locker(mtx_);
        locker.unlock();

        char newFile[LOG_NAME_LEN];
        //日期后缀
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        
        //情况1：时间不匹配，替换为新的日志文件名
        if (toDay_ != t.tm_mday) {
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);
            toDay_ = t.tm_mday;
            lineCount_ = 0;
        } else {
            //情况2：日志行数超出限制，生成新的日志文件名
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s", path_, tail, (lineCount_  / MAX_LINES), suffix_);
        }
        //重新加锁，关闭旧文件，打开新文件
        locker.lock();
        flush();
        fclose(fp_);
        //用"a"模式（追加写入）
        fp_ = fopen(newFile, "a");
        //确保新文件打开成功
        assert(fp_ != nullptr);
    }
    
    {
        std::unique_lock<std::mutex> locker(mtx_);
        lineCount_++;
        //拼接时间戳
        int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);
        buff_.HasWritten(n);
        //添加日志级别
        AppendLogLevelTitle_(level);    

        va_start(vaList, format); //初始化可变参数列表
        //snprintf直接接收可变参数
        //而vsnprintf接收的是va_list类型的参数
        int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);
        va_end(vaList); //结束可变参数的获取

        buff_.HasWritten(m); //缓冲区指针移动
    buff_.Append("\n", 1); // 换行

        //根据模式写入日志
        if(isAsync_ && deque_ && !deque_->full()) { // 异步方式（加入阻塞队列中，等待写线程读取日志信息）
            deque_->push_back(buff_.RetrieveAllToStr());
        } else {    // 同步方式（直接向文件中写入日志信息）
            if (fp_) {
                fputs(buff_.Peek(), fp_);   // 同步就直接写入文件
            }
        }
        buff_.RetrieveAll();    //清空缓冲区，释放空间供下次使用
    }
    
}

// 添加日志等级
void Log::AppendLogLevelTitle_(int level) {
    switch(level) {
    case 0:
        buff_.Append("[debug]: ", 9);
        break;
    case 1:
        buff_.Append("[info] : ", 9);
        break;
    case 2:
        buff_.Append("[warn] : ", 9);
        break;
    case 3:
        buff_.Append("[error]: ", 9);
        break;
    default:
        buff_.Append("[info] : ", 9);
        break;
    }
}

int Log::GetLevel() {
    std::lock_guard<std::mutex> locker(mtx_);
    return level_;
}

void Log::SetLevel(int level) {
    std::lock_guard<std::mutex> locker(mtx_);
    level_ = level;
}