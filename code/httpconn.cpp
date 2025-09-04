//管理服务器与单个客户端之间的 “TCP 连接 + HTTP 通信” 全生命周期
#include "httpconn.h"
#include <errno.h>
using namespace std;

const char* HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() {
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;
}

HttpConn::~HttpConn() {
    Close();
}

void HttpConn::Init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    userCount++; //原子递增在线用户数（统计当前连接数）
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();
    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close() {
    response_.UnmapFile(); //释放响应中通过内存映射的文件资源
    if (isClose_ == false) {
        isClose_ = true;
        userCount--;
        close(fd_); //close为系统调用函数，关闭客户端socket的文件描述符，释放TCP连接
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const {
    return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const {
    return addr_;
}

const char* HttpConn::GetIP() const {
    //addr_.sin_addr存储的是32位整数形式的 IP 地址
    //ntoa = network to ASCII是系统函数，将整数形式的IP转换为字符串形式
    return inet_ntoa(addr_.sin_addr); //返回客户端的IP地址
}

int HttpConn::GetPort() const {
    return ntohs(addr_.sin_port); //返回客户端的端口号
}

//从客户端读取数据到缓冲区
ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1; //存储单次/总读取的字节数，初始化为-1（表示未成功读取）
    //循环读取数据（是否循环取决于触发模式）
    do {
        //调用缓冲区的 ReadFd 方法，从 fd_ 读取数据到 readBuff_
        //参数：fd_ 是客户端 socket 描述符，saveErrno 用于保存错误码
        len = readBuff_.ReadFd(fd_, saveErrno);

        //如果读取的字节数 <= 0（读取失败或无数据），跳出循环
        if (len <= 0) {
            break;
        }
    } while (isET); //循环条件：如果是ET模式，则继续读取直到无数据
    return len; //返回读取的总字节数
}

ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1; //存储单次/总读取的字节数，初始化为-1（表示未成功读取）
    do {
        //调用writev系统调用，将iov_中的两个缓冲区数据发送到fd_
        len = writev(fd_, iov_, iovCnt_);
        
        if (len <= 0) {
            //发送失败：保存错误码到saveErrno，跳出循环
            *saveErrno = errno;
            break; 
        }
        //检查是否所有数据都已发送完毕
        if (iov_[0].iov_len + iov_[1].iov_len  == 0) {
            break; //传输结束
        } else if (static_cast<size_t>(len) > iov_[0].iov_len) {
            //情况1：已发送的数据超过第一个缓冲区（iov_[0]）的长度
            //调整第二个缓冲区（iov_[1]）的指针和长度：减去已发送的部分
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            //如果第一个缓冲区有数据，清空并重置
            if(iov_[0].iov_len) {
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        } else {
            //情况2：已发送的数据未超过第一个缓冲区的长度
            //调整第一个缓冲区的指针和长度：减去已发送的部分
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; 
            iov_[0].iov_len -= len; 
            writeBuff_.Retrieve(len); //从写缓冲区中移除已发送的数据
        }
    } while (isET || ToWriteBytes() > 10240); //循环条件：ET模式或剩余数据量较大
    return len;
}

bool HttpConn::process() {
    request_.Init();
    request_.isET_ = isET;
    // 检查读缓冲区是否有数据
    if (readBuff_.ReadableBytes() <= 0) {
        return false;
    }

    // 步骤1：解析HTTP请求（解析失败返回400错误页面）
    if (!request_.parse(readBuff_, GetFd())) {
        std::string error_path = "/400.html";
        response_.Init(srcDir, error_path, false, 400);
        response_.MakeResponse(writeBuff_);
        
        // 绑定响应缓冲区
        iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
        iov_[0].iov_len = writeBuff_.ReadableBytes();
        iovCnt_ = 1;
        LOG_ERROR("HTTP request parse failed, return 400");
        return true;
    }

    // 步骤2：处理图像上传请求（multipart/form-data + /upload路径）
    if (request_.IsMultipart() && request_.path() == "/upload") {
        const auto& files = request_.GetFiles();
        
        // 打印文件解析结果（关键调试日志）
        LOG_DEBUG("Processing upload request, files count: %zu", files.size());
        for (const auto& [field_name, _] : files) {
            LOG_DEBUG("Found file field: %s", field_name.c_str());
        }

        // 子步骤2.1：无文件上传 → 返回400错误
        if (files.empty()) {
            std::string error_path = "/400.html";
            response_.Init(srcDir, error_path, false, 400);
            response_.MakeResponse(writeBuff_);
            
            iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
            iov_[0].iov_len = writeBuff_.ReadableBytes();
            iovCnt_ = 1;
            LOG_ERROR("No files in upload request, return 400");
            readBuff_.RetrieveAll();
            return true;
        }

        // 子步骤2.2：检查是否存在"file"字段
        auto file_it = files.find("file");
        if (file_it == files.end()) {
            std::string error_path = "/400.html";
            response_.Init(srcDir, error_path, false, 400);
            response_.MakeResponse(writeBuff_);
            
            iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
            iov_[0].iov_len = writeBuff_.ReadableBytes();
            iovCnt_ = 1;
            LOG_ERROR("Missing 'file' field in upload request, return 400");
            return true;
        }

        // 子步骤2.3：获取文件信息并校验类型
        const auto& file_info = file_it->second;
        LOG_DEBUG("Uploaded file: name=%s, type=%s, size=%zu",
                  file_info.filename.c_str(),
                  file_info.content_type.c_str(),
                  file_info.content.size());

        // 宽松校验文件类型（支持jpeg、jpg、png）
        bool is_jpeg = (file_info.content_type == "image/jpeg" || file_info.content_type == "image/jpg");
        bool is_png = (file_info.content_type == "image/png");
        if (!is_jpeg && !is_png) {
            std::string error_path = "/400.html";
            response_.Init(srcDir, error_path, false, 400);
            response_.MakeResponse(writeBuff_);
            
            iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
            iov_[0].iov_len = writeBuff_.ReadableBytes();
            iovCnt_ = 1;
            LOG_ERROR("Unsupported file type: %s, return 400", file_info.content_type.c_str());
            return true;
        }

        // 子步骤2.4：处理图像增强逻辑
        try {
            // 二进制文件内容 → cv::Mat（内存解码）
            std::vector<uchar> image_data(file_info.content.begin(), file_info.content.end());
            cv::Mat input_image = cv::imdecode(image_data, cv::IMREAD_COLOR);
            if (input_image.empty()) {
                throw std::runtime_error("Failed to decode image (corrupted or invalid)");
            }

            // 调用ImageEnhancer增强图像
            cv::Mat enhanced_image = ImageEnhancer::GetInstance().EnhanceImage(input_image);

            // 增强后图像 → JPEG编码
            std::vector<uchar> encoded_data;
            std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 90};
            if (!cv::imencode(".jpg", enhanced_image, encoded_data, encode_params)) {
                throw std::runtime_error("Failed to encode enhanced image");
            }

            // 构建图像响应（直接返回JPEG二进制数据）
            writeBuff_.Append("HTTP/1.1 200 OK\r\n");
            writeBuff_.Append("Content-Type: image/jpeg\r\n");
            writeBuff_.Append("Content-Length: " + std::to_string(encoded_data.size()) + "\r\n");
            
            // 处理长连接
            if (request_.IsKeepAlive()) {
                writeBuff_.Append("Connection: keep-alive\r\n");
                writeBuff_.Append("Keep-Alive: max=6, timeout=120\r\n");
            } else {
                writeBuff_.Append("Connection: close\r\n");
            }
            
            writeBuff_.Append("\r\n");  // 空行分隔头和体
            writeBuff_.Append(encoded_data.data(), encoded_data.size());

            // 绑定响应缓冲区
            iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
            iov_[0].iov_len = writeBuff_.ReadableBytes();
            iovCnt_ = 1;
            LOG_INFO("Image enhanced successfully, size: %zu bytes", encoded_data.size());
            return true;

        } catch (const std::exception& e) {
            // 服务器内部错误（如解码失败、模型错误）→ 返回500
            std::string error_path = "/500.html";
            response_.Init(srcDir, error_path, false, 500);
            response_.MakeResponse(writeBuff_);
            
            iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
            iov_[0].iov_len = writeBuff_.ReadableBytes();
            iovCnt_ = 1;
            LOG_ERROR("Image enhancement failed: %s, return 500", e.what());
            return true;
        }
    }

    // 步骤3：处理普通请求（如访问静态资源）
    LOG_DEBUG("Processing normal request: %s", request_.path().c_str());
    response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    response_.MakeResponse(writeBuff_);

    // 绑定响应缓冲区
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    // 如果有文件内容（如HTML），绑定第二个缓冲区
    if (response_.FileLen() > 0 && response_.File()) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }

    LOG_DEBUG("Response info: filesize=%d, iov_cnt=%d, total_bytes=%d",
              response_.FileLen(), iovCnt_, ToWriteBytes());
    return true;
}

