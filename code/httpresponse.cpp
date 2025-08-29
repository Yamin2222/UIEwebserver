#include "httpresponse.h"

using namespace std;

const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
    { ".html",  "text/html" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain" },
    { ".rtf",   "application/rtf" },
    { ".pdf",   "application/pdf" },
    { ".word",  "application/nsword" },
    { ".png",   "image/png" },
    { ".gif",   "image/gif" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".au",    "audio/basic" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".avi",   "video/x-msvideo" },
    { ".gz",    "application/x-gzip" },
    { ".tar",   "application/x-tar" },
    { ".css",   "text/css"},
    { ".js",    "text/javascript"},
};

const unordered_map<int, string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
};

const unordered_map<int, string> HttpResponse::CODE_PATH = {
    { 400, "/400.html" },
    { 403, "/403.html" },
    { 404, "/404.html" },
};

HttpResponse::HttpResponse() {
    code_ = 1;
    path_ = srcDir_ = "";
    isKeepAlive_ = false;
    mmFile_ = nullptr;
    mmFileStat_ = { 0 };
}

HttpResponse::~HttpResponse() {
    UnmapFile();
}

void HttpResponse::Init(const string& srcDir, string& path,
                        bool isKeepAlive, int code) {
    assert(srcDir != "");
    if (mmFile_) UnmapFile();
    code_ = code;
    isKeepAlive_ = isKeepAlive;
    path_ = path;
    srcDir_ = srcDir;
    mmFile_ = nullptr; 
    mmFileStat_ = { 0 };
}

void HttpResponse::MakeResponse(Buffer& buff) {
    string fullpath = srcDir_ + path_;
    //第一个参数为要查询的文件
    //第二个参数为struct stat 结构体指针，用于存储查询到的文件状态信息
    //成功返回0，并将文件信息写入 buf 结构体，失败返回-1
    //S_ISDIR判断是否是目录
    if (stat(fullpath.c_str(), &mmFileStat_) < 0 
                    || S_ISDIR(mmFileStat_.st_mode)) {
        //情况1：文件不存在（stat 调用失败），或请求的是目录（不是文件）
        code_ = 404;
    //&位判断运算
    //S_IROTH表示 “其他用户是否有读权限”
    } else if (!(mmFileStat_.st_mode & S_IROTH)) {
        //情况2：文件存在，但没有“其他用户可读”权限（S_IROTH 是读权限标志）
        code_ = 403;
    } else if (code_ == -1) {
        //情况3：文件存在且有权限，且之前未设置状态码
        code_ = 200;
    }
    ErrorHtml_();
    AddStateLine_(buff);
    AddHeader_(buff);
    AddContent_(buff);
}

char* HttpResponse::File() {
    return mmFile_;
}

size_t HttpResponse::FileLen() const{
    return mmFileStat_.st_size;
}

void HttpResponse::ErrorHtml_() {
    if (CODE_PATH.count(code_) == 1) {
        path_ = CODE_PATH.find(code_)->second;
        string fullpath = srcDir_ + path_;
        stat(fullpath.c_str(), &mmFileStat_);
    }
}

void HttpResponse::AddStateLine_(Buffer& buff) {
    string status;
    if(CODE_STATUS.count(code_) == 1) {
        status = CODE_STATUS.find(code_)->second;
    }
    else {
        //如果code_ 无效（不在映射表中），默认设为400错误
        code_ = 400;
        status = CODE_STATUS.find(400)->second;
    }
    buff.Append("HTTP/1.1 " + to_string(code_) + " " + status + "\r\n");
}

void HttpResponse::AddHeader_(Buffer& buff) {
    buff.Append("Connection: ");
    if(isKeepAlive_) {
        buff.Append("keep-alive\r\n");
        buff.Append("Keep-Alive: max=6, timeout=120\r\n");
    } else{
        buff.Append("close\r\n");
    }
    buff.Append("Content-type: " + GetFileType_() + "\r\n");
}

void HttpResponse::AddContent_(Buffer& buff) {
    string fullpath = srcDir_ + path_;
    //采用C风格的open打开文件，而不用ifstream
    //因为mmap函数需要文件描述符（int 类型）
    //O_RDONLY为fcntl.h头文件的一个宏定义，指定文件的打开模式（只读）
    int srcFd = open(fullpath.c_str(), O_RDONLY); 

    if(srcFd < 0) { //如果打开失败
        ErrorContent(buff, "File NotFound!");
        return; 
    }

    LOG_DEBUG("file path %s", fullpath.c_str()); //打印日志
    //将文件映射到内存（高效读取文件内容）
    // 参数说明：
    // 0：让系统自动分配内存地址
    // mmFileStat_.st_size：映射的文件大小（从之前获取的文件状态中获取）
    // PROT_READ：内存区域权限为“只读”
    // MAP_PRIVATE：建立私有映射（修改内存不会影响原文件）
    // srcFd：已打开的文件描述符
    // 0：从文件开头开始映射
    if (mmFileStat_.st_size > 0) {
        void* mapRet = mmap(nullptr, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
        if (mapRet == MAP_FAILED) {
            // 映射失败，关闭文件并返回错误内容
            close(srcFd);
            code_ = 404;
            ErrorContent(buff, "File NotFound!");
            return;
        }
        // 保存内存映射的地址
        mmFile_ = static_cast<char*>(mapRet);
    }
    // 关闭文件描述符（无论是否映射）
    close(srcFd);
    //添加Content-length响应头
    buff.Append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}

//释放内存映射资源
void HttpResponse::UnmapFile() {
    if(mmFile_) {
        munmap(mmFile_, mmFileStat_.st_size);
        //将指针置空避免野指针
        mmFile_ = nullptr;
    }
}

// 判断文件类型 
string HttpResponse::GetFileType_() {
    //查找最后一个点，用于查找后缀
    string::size_type idx = path_.find_last_of('.');
    if(idx == string::npos) {   
        return "text/plain"; //没找到，默认按纯文本处理
    }
    string suffix = path_.substr(idx); //找到后缀后分割后缀
    if(SUFFIX_TYPE.count(suffix) == 1) {
        return SUFFIX_TYPE.find(suffix)->second; //从后缀表中查找对应类型返回
    }
    return "text/plain"; ////从后缀表中无对应类型则按纯文本处理
}

//生成错误响应内容
void HttpResponse::ErrorContent(Buffer& buff, string message) 
{
    string body;
    string status;
    body += "<html><title>Error</title>";
    body += "<body bgcolor=\"ffffff\">";
    if(CODE_STATUS.count(code_) == 1) {
        status = CODE_STATUS.find(code_)->second;
    } else {
        status = "Bad Request";
    }
    body += to_string(code_) + " : " + status  + "\n";
    body += "<p>" + message + "</p>";
    body += "<hr><em>UIEWebServer</em></body></html>";

    buff.Append("Content-length: " + to_string(body.size()) + "\r\n\r\n");
    buff.Append(body);
}