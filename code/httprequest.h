#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex> //正则表达式
#include <errno.h>
#include <mysql/mysql.h>

#include "buffer.h"
#include "log.h"
#include "sqlconnpool.h"

// 新增：存储上传文件的信息（文件名 + 二进制内容 + MIME类型）
struct FileInfo {
    std::string filename;    // 文件名（如 "test.jpg"）
    std::string content;     // 文件二进制内容（直接存储字节流）
    std::string content_type;// MIME类型（如 "image/jpeg"、"image/png"）
};

class HttpRequest {
public:
    //请求状态
    enum PARSE_STATE {
        REQUEST_LINE, //正在解析 请求行
        HEADERS, //正在解析 请求头
        BODY, //正在解析 请求体
        FINISH, //解析完成
    };

    // 新增：获取上传的文件列表（对外提供只读访问）
    const std::unordered_map<std::string, FileInfo>& GetFiles() const {
        return files_;
    }

    // 新增：判断当前请求是否为 multipart/form-data 格式（文件上传）
    bool IsMultipart() const {
        auto it = header_.find("Content-Type");
        return (it != header_.end()) && (it->second.find("multipart/form-data") != std::string::npos);
    }

    HttpRequest() {
        Init();
    }
    ~HttpRequest() = default;

    void Init();
    //核心解析函数
    //从缓冲区 buff 中读取 HTTP 请求数据，按状态逐步解析
    bool parse(Buffer& buff); 

    std::string path() const; //获取请求路径，不可修改（如 /login.html）
    std::string& path(); //获取请求路径的引用，可修改
    std::string method() const; //获取HTTP请求方法
    std::string version() const; //获取HTTP版本
    std::string GetPost(const std::string& key) const; //从POST表单数据中获取指定key的值
    std::string GetPost(const char* key) const;

    bool IsKeepAlive() const; //判断当前请求是否需要“长连接”

private:
    bool ParseRequestLine_(const std::string& line); //处理请求行
    void ParseHeader_(const std::string& line); //处理请求头
    void ParseBody_(const std::string& line); //处理请求体

    void ParsePath_(); //处理请求路径
    void ParsePost_(); //处理Post事件
    void ParseFormUrlencoded_(); //从url解析编码

    //用户验证
    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);

    // 新增：解析 multipart/form-data 格式的请求体（核心新增方法）
    void ParseMultipartBody_();
    // 新增：提取 multipart 分隔符（如 "boundary=----WebKitFormBoundaryxxxx" 中的分隔符）
    std::string GetMultipartBoundary_() const;

    PARSE_STATE state_; //当前请求的解析状态
    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_; //请求头，由头部字段名和值组成
    std::unordered_map<std::string, std::string> post_;

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    static int ConverHex(char ch);

    // 新增：存储上传的文件（key：表单字段名，如前端 <input name="file"> 的 "file"；value：FileInfo）
    std::unordered_map<std::string, FileInfo> files_;
    // 新增：multipart 分隔符（如 "----WebKitFormBoundaryxxxx"）
    std::string multipart_boundary_;
};

#endif