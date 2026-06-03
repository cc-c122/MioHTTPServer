# MioHTTPServer

一个使用 C++17 实现的 Linux HTTP/1.1 服务器学习项目，核心目标是练习非阻塞 I/O、epoll、线程池、连接状态机和 RAII 资源管理。

当前代码实现了一个迷你 HTTP server 的主体结构，包括连接接入、请求解析、响应生成、keep-alive、超时清理和基础路由。

## 功能特性

- Linux `epoll` 事件循环
- Edge Triggered + OneShot 事件模型
- 非阻塞 socket
- 线程池处理请求
- HTTP/1.1 请求解析
- Keep-Alive 支持
- RAII 文件描述符封装
- 读写锁保护连接表
- 空闲连接超时清理
- 简单内置路由
  - `/`
  - `/index.html`
  - `/health`

## 项目结构

```text
.
├── CMakeLists.txt
├── http_server.h
├── http_server.cpp
└── test.cpp
```

## 环境要求

- Linux 或 WSL
- CMake 3.10+
- 支持 C++17 的编译器
  - GCC
  - Clang

该项目使用 `epoll`、`fcntl`、`unistd`、`pthread` 等 Linux/POSIX API，不适合直接在原生 Windows 环境编译运行。

## 构建

```bash
cmake -S . -B build
cmake --build build
```

按照当前 `CMakeLists.txt` 配置，输出文件会生成到仓库根目录的 `bin/` 下。

## 运行

默认端口为 `8080`，默认线程数为 `4`：

```bash
./bin/server
```

也可以指定端口和线程数：

```bash
./bin/server 8080 4
```

## 测试

启动服务后，可以用浏览器或 `curl` 访问：

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/health
```

## 当前状态

这是一个偏学习和实验性质的项目，重点在于搭建 HTTP server 的并发 I/O 骨架。后续可以继续完善：

- 修复和清理现有拼写问题
- 增加完整编译测试
- 增加静态文件服务
- 增加更完整的 HTTP request/response 处理
- 增加单元测试和压测脚本
- 增加优雅关闭和错误处理细节
