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
bool HttpRequest::parse(Buffer& buff) {
    const char CRLF[] = "\r\n";
    if (buff.ReadableBytes() <= 0) {
        return false;
    }

    // 核心思路：
    // 1. 先解析请求行（REQUEST_LINE）和请求头（HEADERS）（按行解析）；
    // 2. 解析完请求头后，根据 Content-Length 读取完整的请求体（二进制，不按行分割）；
    // 3. 最后解析请求体（BODY）。
    while (buff.ReadableBytes() && state_ != FINISH) {
        if (state_ != BODY) { // 非 BODY 状态：按行解析（请求行、请求头）
            const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
            std::string line(buff.Peek(), lineEnd);

            switch (state_) {
                case REQUEST_LINE:
                    if (!ParseRequestLine_(line)) {
                        return false;
                    }
                    ParsePath_();
                    state_ = HEADERS; // 解析完请求行，进入请求头状态
                    break;
                case HEADERS:
                    ParseHeader_(line);
                    // 检查请求头是否结束：空行（仅 CRLF）表示请求头结束
                    if (line.empty()) {
                        // 请求头结束后，判断是否有请求体（通过 Content-Length 判断）
                        if (header_.count("Content-Type") && 
                            header_["Content-Type"].find("multipart/form-data") != std::string::npos) {
                            // 是文件上传请求，需要读取请求体（进入 BODY 状态）
                            state_ = BODY;
                        } else {
                            // 无请求体，直接完成解析
                            state_ = FINISH;
                        }
                    }
                    break;
                default:
                    break;
            }

            if (lineEnd == buff.BeginWriteConst()) {
                break; // 缓冲区已读完，等待新数据
            }
            buff.RetrieveUntil(lineEnd + 2); // 跳过当前行的 CRLF
        } else { // BODY 状态：读取完整的二进制请求体（不按行分割）
            // 1. 从请求头获取请求体长度（Content-Length）
            size_t content_len = 0;
            if (header_.count("Content-Length")) {
                try {
                    content_len = stoull(header_["Content-Length"]); // 字符串转无符号长整型
                } catch (...) {
                    LOG_ERROR("Invalid Content-Length: %s", header_["Content-Length"].c_str());
                    return false;
                }
            } else {
                LOG_ERROR("No Content-Length in multipart request");
                return false;
            }

            // 2. 读取足够的请求体数据（直到达到 Content-Length）
            size_t readable = buff.ReadableBytes();
            if (readable < content_len) {
                break; // 数据不足，等待新数据（epoll 会继续触发可读事件）
            }

            // 3. 读取完整的请求体（二进制数据）
            body_.assign(buff.Peek(), content_len);
            buff.Retrieve(content_len); // 从缓冲区中移除已读取的数据

            // 4. 解析请求体（multipart 格式）
            ParseBody_(body_); // 传入完整的二进制请求体，而非单行文本
            state_ = FINISH; // 解析完请求体，完成整个请求解析
        }
    }

    LOG_DEBUG("[%s], [%s], [%s], Content-Length: %zu", 
              method_.c_str(), path_.c_str(), version_.c_str(), body_.size());
    return true;
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
    //^匹配字符串的开始位置，在[]中则表示“非”，表示不接受方括号表达式中的字符
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    smatch subMatch; //用于存储正则匹配到的结果（捕获组）
    if (regex_match(line, subMatch, patten)) {
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = HEADERS;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

void HttpRequest::ParseHeader_(const string& line) {
    //.表示任意字符
    regex patten("^([^:]*): ?(.*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        header_[subMatch[1]] = subMatch[2];
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