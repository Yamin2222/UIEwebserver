#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "buffer.h"
#include "log.h"

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    //初始化函数
    void Init(const std::string& srcDir_, std::string& path, 
        bool isKeepAlive = false, int code = -1);

    void MakeResponse(Buffer& buff); //核心生成函数

    void UnmapFile(); //释放内存映射（mmFile_）
    char* File(); //返回mmFile_指针
    size_t FileLen() const;
    void ErrorContent(Buffer& buff, std::string message); //错误处理函数
    int Code() const {
        return code_;
    }

private:
    void AddStateLine_(Buffer& buff);
    void AddHeader_(Buffer& buff);
    void AddContent_(Buffer& buff);

    void ErrorHtml_();
    std::string GetFileType_();

    int code_; //状态码
    bool isKeepAlive_; //是否长连接

    std::string path_; //响应资源的路径（如/index.html，即服务器要返回的文件路径）
    std::string srcDir_; //服务器静态资源的根目录（如./www，path_是相对于srcDir_ 的路径）

    char* mmFile_; //内存映射文件的指针
    struct stat mmFileStat_; //存储内存映射文件的状态

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE; //文件后缀名与MIME类型的映射
    static const std::unordered_map<int, std::string> CODE_STATUS; //状态码与状态描述的映射
    static const std::unordered_map<int, std::string> CODE_PATH; //状态码与错误页面路径的映射
};

#endif