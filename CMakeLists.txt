cmake_minimum_required(VERSION 3.10)
project(web_server)

# 设置C++标准（你的代码使用了C++17特性，需要设置为17）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项：开启优化、警告和调试信息
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -g")

# 查找源文件
file(GLOB CODE_SOURCES ./code/*.cpp)
set(MAIN_SOURCE ./main.cpp)
set(ALL_SOURCES ${CODE_SOURCES} ${MAIN_SOURCE})

# 1. 查找 OpenCV 库（关键：添加OpenCV支持）
find_package(OpenCV REQUIRED)
if(NOT OpenCV_FOUND)
    message(FATAL_ERROR "OpenCV not found. Please install OpenCV first.")
endif()

# 2. 配置 ONNX Runtime（关键：添加ONNX Runtime支持）
# 手动指定ONNX Runtime的安装路径（根据你的实际路径修改）
set(ONNX_RUNTIME_DIR "/home/yamin/onnxruntime/onnxruntime-linux-x64-gpu-1.16.3")
include_directories(${ONNX_RUNTIME_DIR}/include)  # 添加头文件目录
link_directories(${ONNX_RUNTIME_DIR}/lib)         # 添加库文件目录

# 3. 查找线程库
find_package(Threads REQUIRED)

# 4. 查找MySQL客户端库
find_library(MYSQL_CLIENT_LIB mysqlclient)
if(NOT MYSQL_CLIENT_LIB)
    message(FATAL_ERROR "mysqlclient library not found. Please install libmysqlclient-dev.")
endif()

# 指定可执行文件输出目录
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# 创建可执行文件
add_executable(server ${ALL_SOURCES})

# 链接所有依赖库（包括OpenCV、ONNX Runtime等）
target_link_libraries(server 
    Threads::Threads 
    ${MYSQL_CLIENT_LIB}
    ${OpenCV_LIBS}  # 链接OpenCV库
    onnxruntime     # 链接ONNX Runtime库
)

# 传递资源目录定义
target_compile_definitions(server PRIVATE RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")
