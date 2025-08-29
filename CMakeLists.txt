cmake_minimum_required(VERSION 3.10)
project(web_server)

# 设置C++标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项：开启优化、警告和调试信息
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -g")

# 查找源文件
# 查找code目录下所有的.cpp文件
file(GLOB CODE_SOURCES ./code/*.cpp)
# 主程序入口文件
set(MAIN_SOURCE ./main.cpp)

# 合并所有源文件
set(ALL_SOURCES ${CODE_SOURCES} ${MAIN_SOURCE})

# 查找线程库（用于线程池）
find_package(Threads REQUIRED)

# 查找MySQL客户端库（如果项目使用了MySQL）
find_library(MYSQL_CLIENT_LIB mysqlclient)
if(NOT MYSQL_CLIENT_LIB)
    message(FATAL_ERROR "mysqlclient library not found. Please install libmysqlclient-dev.")
endif()

# 指定可执行文件输出目录（在当前目录下创建bin文件夹）
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# 创建可执行文件
add_executable(server ${ALL_SOURCES})

# 链接依赖库
target_link_libraries(server 
    Threads::Threads  # 链接线程库
    ${MYSQL_CLIENT_LIB}  # 链接MySQL客户端库
)