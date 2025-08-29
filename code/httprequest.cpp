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
    const char CRLF[] = "\r\n"; //行结束标志
    if (buff.ReadableBytes() <= 0) { //没有可读字节
        return false;
    }

    //读取数据
    //循环条件：缓冲区有可读数据，且解析状态未完成（state_ != FINISH）
    while (buff.ReadableBytes() && state_ != FINISH) {
        //按行分割的流式数据
        //步骤1：查找当前行的结束位置,四个参数都是指针
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        //步骤2：提取当前行的内容（从缓冲区当前位置到lineEnd，不包含CRLF）
        std::string line(buff.Peek(), lineEnd);
        switch (state_) {
            //有限状态机，从请求行开始，每处理完后会自动转入到下一个状态
            case REQUEST_LINE: //状态1：解析请求行
                if (!ParseRequestLine_(line)) {
                    return false;
                }
                ParsePath_();
                break;
            case HEADERS: //状态2：解析请求头
                ParseHeader_(line);
                //检查请求头是否解析完毕：请求头结束后会有一个空行（仅CRLF）
                //此时缓冲区剩余可读字节 <= 2（刚好一个CRLF），说明请求头结束
                if (buff.ReadableBytes() <= 2) {
                    state_ = FINISH;
                }
                break;
            case BODY: //状态3：解析请求体
                ParseBody_(line);
                break;
            case FINISH: //状态4：已完成解析
                break;
            default:
                break;
        }
        //步骤4：判断是否已读完缓冲区数据
        //lineEnd 等于缓冲区末尾，说明当前缓冲区数据已读完，跳出循环（等待新数据）
        if (lineEnd == buff.BeginWriteConst()) {
            break; //读完了
        }
        buff.RetrieveUntil(lineEnd + 2);
    }
    //c_str()是将string转换为C风格字符串（const char*）
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

//解析简化路径（如果需要）
void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    }
    else {
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
    else {
        state_ = BODY;  // 状态转换为下一个状态
    }
}

//请求体是请求中携带的实际数据，通常在POST请求中使用
void HttpRequest::ParseBody_(const string& line) {
    body_ = line;
    ParsePost_();
    state_ = FINISH;    // 状态转换为下一个状态
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

// 16进制转化为10进制
int HttpRequest::ConverHex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return 0;
}

void HttpRequest::ParsePost_() {
    //POST请求且为表单数据
    if (method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFormUrlencoded_();
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