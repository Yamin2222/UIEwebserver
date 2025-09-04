#include "httprequest.h"
#include <algorithm>
using namespace std;

//服务器默认支持的HTML页面路径
//快速判断某个请求路径是否是 “默认 HTML 页面”
const unordered_set<string> HttpRequest::DEFAULT_HTML {
    "/index", "/register", "/login",
    "/welcome", "/video", "/picture",
};

const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
    {"/register.html", 0}, {"/login.html", 1}, 
};

void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    header_.clear();
    post_.clear();
    isET_ = false; // 默认非ET模式
}

//判断是否需要长连接，两个条件：
//请求头中存在 Connection: keep-alive
//HTTP 版本是 1.1
bool HttpRequest::IsKeepAlive() const {
    if (header_.count("Connection") == 1) {
        return header_.find("Connection")->second == "keep-alive" && version_ == "1.1"; 
    }
    return false;
}

//核心解析函数
//从缓冲区buff中读取 HTTP 请求数据，按状态逐步解析
bool HttpRequest::parse(Buffer& buff, int fd) {
    const char CRLF[] = "\r\n";
    bool is_parse_continue = true; // 标记是否需要继续解析（无错误）

    // 1. 解析请求行（仅在 REQUEST_LINE 状态处理）
    if (state_ == REQUEST_LINE && buff.ReadableBytes() > 0) {
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        
        // 情况1：请求行不完整（未找到 CRLF），等待更多数据
        if (lineEnd == buff.BeginWriteConst()) {
            LOG_DEBUG("RequestLine incomplete, wait for more data");
            return is_parse_continue; // 返回 true，无错误
        }

        // 情况2：请求行完整，解析
        std::string requestLine(buff.Peek(), lineEnd);
        if (!ParseRequestLine_(requestLine)) {
            LOG_ERROR("RequestLine Error: invalid format -> [%s]", requestLine.c_str());
            is_parse_continue = false; // 解析出错，返回 false
        } else {
            // 解析成功：移动缓冲区指针，切换到 HEADERS 状态
            buff.RetrieveUntil(lineEnd + 2);
            state_ = HEADERS;
            ParsePath_(); // 处理路径（如 / → /index.html）
        }
    }

    // 2. 解析请求头（仅在 HEADERS 状态处理，且上一步无错误）
    if (state_ == HEADERS && is_parse_continue && buff.ReadableBytes() > 0) {
        while (true) {
            const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
            
            // 情况1：当前行不完整，等待更多数据
            if (lineEnd == buff.BeginWriteConst()) {
                LOG_DEBUG("Headers incomplete, wait for more data");
                break;
            }

            // 情况2：当前行完整，提取并解析
            std::string line(buff.Peek(), lineEnd);
            buff.RetrieveUntil(lineEnd + 2);

            // 空行 → 请求头结束，切换状态
            if (line.empty()) {
                if (method_ == "POST") {
                    // POST 请求：根据 Content-Type 判断是否需要解析 body
                    if (header_.count("Content-Type") && 
                        header_["Content-Type"].find("multipart/form-data") != std::string::npos) {
                        state_ = BODY; // 需要解析 multipart body
                    } else {
                        state_ = FINISH; // 无 body 或非 multipart，解析完成
                    }
                } else {
                    // GET 等请求：无 body，解析完成
                    state_ = FINISH;
                }
                break;
            } else {
                // 非空行 → 解析请求头（如 Content-Type、Content-Length）
                ParseHeader_(line);
            }
        }
    }

    // 3. 解析请求体（仅在 BODY 状态处理，且上一步无错误）
    if (state_ == BODY && is_parse_continue) {
        // 第一步：检查是否有 Content-Length（multipart 必须包含）
        if (header_.count("Content-Length") == 0) {
            LOG_ERROR("POST request missing Content-Length header");
            is_parse_continue = false;
            return is_parse_continue;
        }

        // 第二步：解析 Content-Length（防止无效值）
        size_t content_length = 0;
        try {
            content_length = std::stoull(header_["Content-Length"]);
        } catch (...) {
            LOG_ERROR("Invalid Content-Length: %s", header_["Content-Length"].c_str());
            is_parse_continue = false;
            return is_parse_continue;
        }

        // 第三步：确保读取到完整的 body 数据（核心：适配 ET 模式）
        while (buff.ReadableBytes() < content_length) {
            // 非 ET 模式：数据不完整，等待下次触发（不主动读）
            if (!isET_) {
                LOG_DEBUG("Non-ET mode: body incomplete (current: %zu, need: %zu)",
                          buff.ReadableBytes(), content_length);
                break;
            }

            // ET 模式：主动循环读取，直到数据足够或出错
            ssize_t read_len = buff.ReadFd(fd, nullptr); // 需 Buffer 类支持 GetFd()
            if (read_len <= 0) {
                // 读取失败（EAGAIN 是正常情况，其他是错误）
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    LOG_DEBUG("ET mode: no more data (EAGAIN), wait for next event");
                    break;
                } else {
                    LOG_ERROR("ET mode: read body failed, errno: %d", errno);
                    is_parse_continue = false;
                    return is_parse_continue;
                }
            }
            LOG_DEBUG("ET mode: read %zd bytes, total readable: %zu",
                      read_len, buff.ReadableBytes());
        }

        // 第四步：数据足够，解析 body
        if (buff.ReadableBytes() >= content_length) {
            body_.assign(buff.Peek(), content_length);
            buff.Retrieve(content_length); // 移动缓冲区指针，释放已解析数据
            ParseBody_(body_); // 解析 multipart 数据（提取文件）
            state_ = FINISH; // 标记解析完成
            LOG_DEBUG("Body parsed successfully, size: %zu bytes", body_.size());
        } else {
            LOG_DEBUG("Body still incomplete, wait for more data");
        }
    }

    // 返回值语义：true = 无错误（可继续解析），false = 解析出错
    return is_parse_continue;
}

//解析简化路径（如果需要）
void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    } else if (path_ == "/enhance") {
        return;
    } else {
        for(auto &item: DEFAULT_HTML) {
            if(item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}

bool HttpRequest::ParseRequestLine_(const string& line) {
    // 优化后正则：
    // 1. ([A-Z]+)：仅匹配大写请求方法（POST/GET/PUT等，符合HTTP规范）
    // 2. \\s+：匹配1个及以上空格（兼容浏览器连续空格）
    // 3. ([^ ]+)：匹配路径（无空格，如/upload）
    // 4. HTTP/([0-9.]+)：严格匹配版本号（如1.1、1.0）
    regex patten("^([A-Z]+)\\s+([^ ]+)\\s+HTTP/([0-9.]+)$");
    smatch subMatch;

    if (regex_match(line, subMatch, patten)) {
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = HEADERS;
        LOG_DEBUG("Parsed RequestLine: method=%s, path=%s, version=%s",
                  method_.c_str(), path_.c_str(), version_.c_str());
        return true;
    }

    // 打印错误请求行内容，方便定位格式问题
    LOG_ERROR("RequestLine Error: invalid format -> [%s]", line.c_str());
    return false;
}

void HttpRequest::ParseHeader_(const string& line) {
    //.表示任意字符
    regex patten("^([^:]*): ?(.*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        header_[subMatch[1]] = subMatch[2];
                LOG_DEBUG("Header: %s: %s", subMatch[1].str().c_str(), subMatch[2].str().c_str());
    }
    // else {
    //     state_ = BODY;  // 状态转换为下一个状态
    // }
}

//请求体是请求中携带的实际数据，通常在POST请求中使用
void HttpRequest::ParseBody_(const std::string& full_body) {
    body_ = full_body; // 关键：直接用完整的二进制请求体覆盖 body_（而非 +=）
    ParsePost_(); // 解析 multipart 数据
    state_ = FINISH;
    // 日志：打印实际的请求体长度和是否为 multipart 类型
    LOG_DEBUG("Body len:%zu, is_multipart:%d, boundary:%s", 
              body_.size(), IsMultipart() ? 1 : 0, 
              multipart_boundary_.c_str());
}

// 16进制转化为10进制
int HttpRequest::ConverHex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return 0;
}

void HttpRequest::ParsePost_() {
    if (method_ != "POST") {
        return;
    }

    // 分支1：multipart/form-data（文件上传，如图像）
    if (IsMultipart()) {
        // 先获取并存储 multipart 分隔符
        multipart_boundary_ = GetMultipartBoundary_();
        if (multipart_boundary_.empty()) {
            LOG_ERROR("Multipart boundary not found");
            return;
        }
        // 解析 multipart 格式的请求体，提取文件
        ParseMultipartBody_();
        // 解析后检查 files_ 是否为空（无文件）
        if (files_.empty()) {
            LOG_ERROR("No files parsed from multipart request");
            state_ = FINISH; // 强制结束解析
        }
        return;
    }

    // 分支2：application/x-www-form-urlencoded（普通表单，如登录/注册）
    if (header_.count("Content-Type") && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFormUrlencoded_();
        // 原有用户验证逻辑（登录/注册）保留不变
        if (DEFAULT_HTML_TAG.count(path_)) {
            int tag = DEFAULT_HTML_TAG.find(path_)->second;
            LOG_DEBUG("Tag:%d", tag);
            if (tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if (UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } else {
                    path_ = "/error.html";
                }
            }
        }
    }
}

// 1. 提取 multipart 分隔符（从 Content-Type 中获取，如 "boundary=----WebKitFormBoundaryxxxx"）
std::string HttpRequest::GetMultipartBoundary_() const {
    auto it = header_.find("Content-Type");
    if (it == header_.end()) {
        return "";
    }
    const std::string& content_type = it->second;
    // 匹配 "boundary=" 后的分隔符（支持可选的引号，如 boundary="----xxx"）
    std::regex patten("boundary=(\"?)([^\";]+)\\1");
    std::smatch subMatch;
    if (regex_search(content_type, subMatch, patten) && subMatch.size() >= 3) {
        // 分隔符需在前后加 "--"（前端传递的是 "----xxx"，解析后需补全为 "--xxxx" 用于分割）
        return "--" + subMatch[2].str();
    }
    return "";
}

// 2. 解析 multipart/form-data 格式的请求体，提取文件信息
void HttpRequest::ParseMultipartBody_() {
    if (body_.empty() || multipart_boundary_.empty()) {
        return;
    }

    const std::string& boundary = multipart_boundary_;
    const std::string end_boundary = boundary + "--"; // 结束分隔符（比普通分隔符多一个 "--"）
    const size_t boundary_len = boundary.size();
    const size_t end_boundary_len = end_boundary.size();

    size_t pos = 0;
    const size_t body_len = body_.size();

    // 循环解析每个文件块（每个块以 boundary 开头，以 boundary 或 end_boundary 结尾）
    while (pos < body_len) {
        // 步骤1：找到当前块的边界（从 pos 开始找 boundary）
        size_t boundary_pos = body_.find(boundary, pos);
        if (boundary_pos == std::string::npos) {
            break; // 未找到边界，解析结束
        }
        pos = boundary_pos + boundary_len; // 跳过边界，指向块内容开始位置

        // 步骤2：判断是否为结束边界（若到末尾，退出循环）
        if (body_.substr(pos, end_boundary_len) == end_boundary) {
            break;
        }

        // 步骤3：解析块的头部（包含文件名、MIME类型、表单字段名）
        FileInfo file_info;
        std::string field_name; // 表单字段名（如前端 <input name="file"> 的 "file"）
        bool is_file = false;

        // 解析块头部的每一行（直到遇到空行，空行后是文件二进制内容）
        while (pos < body_len) {
            // 找当前行的结束（CRLF）
            size_t line_end = body_.find("\r\n", pos);
            if (line_end == std::string::npos) {
                break;
            }
            std::string line = body_.substr(pos, line_end - pos);
            pos = line_end + 2; // 跳过 CRLF

            // 空行：头部解析结束，后续是文件二进制内容
            if (line.empty()) {
                break;
            }

            // 解析 "Content-Disposition" 行（包含表单字段名、文件名）
            if (line.find("Content-Disposition") != std::string::npos) {
                // 匹配表单字段名：name="file"
                std::regex name_patten("name=\"([^\"]+)\"");
                std::smatch name_match;
                if (regex_search(line, name_match, name_patten) && name_match.size() >= 2) {
                    field_name = name_match[1].str();
                }
                // 匹配文件名：filename="test.jpg"（存在 filename 说明是文件，否则是普通表单字段）
                std::regex filename_patten("filename=\"([^\"]+)\"");
                std::smatch filename_match;
                if (regex_search(line, filename_match, filename_patten) && filename_match.size() >= 2) {
                    file_info.filename = filename_match[1].str();
                    is_file = true; // 标记为文件类型
                }
            }

            // 解析 "Content-Type" 行（文件的 MIME 类型，如 image/jpeg）
            if (is_file && line.find("Content-Type") != std::string::npos) {
                std::regex type_patten("Content-Type: ([^;]+)");
                std::smatch type_match;
                if (regex_search(line, type_match, type_patten) && type_match.size() >= 2) {
                    file_info.content_type = type_match[1].str();
                }
            }
        }

        // 步骤4：解析文件二进制内容（从当前 pos 到下一个 boundary 之前）
        if (is_file && !field_name.empty()) {
            // 找到下一个 boundary 的位置（内容结束位置）
            size_t content_end = body_.find(boundary, pos);
            if (content_end == std::string::npos) {
                content_end = body_len; // 若未找到，取 body 末尾
            }
            // 提取二进制内容（注意：部分浏览器会在内容末尾加 CRLF，需去掉）
            size_t content_len = content_end - pos;
            if (content_len > 2 && body_.substr(content_end - 2, 2) == "\r\n") {
                content_len -= 2; // 去掉末尾的 CRLF
            }
            file_info.content = body_.substr(pos, content_len);
            // 将文件信息存入 files_ 容器（key：表单字段名）
            files_[field_name] = file_info;
            LOG_DEBUG("Multipart file: field=%s, name=%s, size=%zu, type=%s",
                      field_name.c_str(), file_info.filename.c_str(),
                      file_info.content.size(), file_info.content_type.c_str());
        }

        // 移动 pos 到下一个 boundary 位置，准备解析下一个块
        pos = body_.find(boundary, pos);
        if (pos == std::string::npos) {
            break;
        }
    }
}

//专门用于解析application/x-www-form-urlencoded格式的POST表单数据
//核心作用是将形如username=zhangsan&password=123的编码字符串，解析成键值对
//首先要明确URL编码：
//1.空格会被编码为+或%20；
//2.其他特殊字符（如 ?、&、中文等）会被编码为%+十六进制数
//（如%E4%B8%AD是“中” 的编码）
//3.键值对之间用&分隔，键和值之间用=分隔
void HttpRequest::ParseFormUrlencoded_() {
    if (body_.size() == 0) return;

    string key, value;
    int num  = 0;
    int n = body_.size();
    int i = 0, j = 0;

    for (; i <n; i++) {
        char ch = body_[i];
        switch (ch) {
            case '=': //遇到“=”：前面的部分是键（key）
                key = body_.substr(j, i-j);
                j = i + 1;
                break;
            case '+': //遇到“+”：URL编码中“+”代表空格，替换为空格
                body_[i] = ' ';
                break;
            case '%': //遇到“%”：URL编码的特殊字符（如%20代表空格）
                //解析%后面的两个十六进制字符（如%20中，20是十六进制）
                num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
                // body_[i + 2] = num % 10 + '0';
                // body_[i + 1] = num / 10 + '0';
                body_[i] = static_cast<char>(num);
                i += 2;
                break;
            case '&': //遇到“&”：前面的部分是值（value），且一组键值对结束
                value = body_.substr(j, i - j);
                j = i + 1;
                post_[key] = value; //将键值对存入post_哈希表
                LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
                break;
            default:
                break;
        }
    }
    assert(j <= i);
    if (post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

bool HttpRequest::UserVerify(const string& name, const string& pwd, bool isLogin) {
    if (name == "" || pwd == "") return false;
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII(&sql, SqlConnPool::Instance());
    assert(sql);

    bool flag = false; //验证结果，默认失败
    if (!isLogin) flag = true; //如果是注册，默认可以注册

    unsigned int j = 0; //获取字段数量
    char order[256] = { 0 }; //存储sql语句

    MYSQL_FIELD *fields = nullptr; //字段信息
    MYSQL_RES *res = nullptr; //查询结果

    //构建sql核心查询语句
    //查询指令，意思是：“去user表（用户表）里找username等于name的记录，只找 1 条”。
    snprintf(order,256, 
        "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    LOG_DEBUG("%s", order);
    
    //执行sql语句
    if(mysql_query(sql, order)) { 
        mysql_free_result(res); //执行失败，释放资源
        return false; 
    }

    res = mysql_store_result(sql); //存储查询结果
    j = mysql_num_fields(res); //获取字段数量（username和password两个字段）
    fields = mysql_fetch_fields(res); //获取字段信息

    // 遍历查询结（如果查到了记录）
    while(MYSQL_ROW row = mysql_fetch_row(res)) { //逐行读取结果
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]); //打印查到的用户名和密码
        string password(row[1]);//从结果中取出密码（row[0]是用户名，row[1]是密码） 
         
        //如果是登录操作
        if(isLogin) {
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_INFO("pwd error!");
            }
        } 
        //如果是注册操作（此时查到了同名用户）
        else { 
            flag = false; 
            LOG_INFO("user used!");
        }
    }
    mysql_free_result(res); //释放查询结果

    if(!isLogin && flag == true) { //注册成功
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; 
        }
        flag = true;
    }
    LOG_DEBUG( "UserVerify success!!");
    return flag;
}   

string HttpRequest::path() const {
    return path_;
}

string& HttpRequest::path(){
    return path_;
}

string HttpRequest::method() const {
    return method_;
}

string HttpRequest::version() const {
    return version_;
}

string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}