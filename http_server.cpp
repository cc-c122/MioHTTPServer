#include "http_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <fstream>
#include <filesystem>

//HttpRequest 实现
bool HttpRequest::parse(const std::string& raw){
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos){
        return false;
    }
    
    std::string headerPart = raw.substr(0, headerEnd);
    std::istringstream iss(headerPart);
    std::string line;
    
    if (!std::getline(iss, line) || line.empty()){
        return false;
    }
    
    if (line.back() == '\r') line.pop_back();
    
    std::istringstream lineStream(line);
    if (!(lineStream >> method >> path >> version)){
        return false;
    }
    
    while (std::getline(iss, line) && !line.empty() && line != "\r"){
        if (line.back() == '\r') line.pop_back();
        
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos){
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            headers[key] = value;
        }
    }
    
    auto it = headers.find("content-length");
    if (it != headers.end()){
        size_t contentLength = std::stoul(it->second);
        size_t bodyStart = headerEnd + 4;
        if (raw.length() >= bodyStart + contentLength){
            body = raw.substr(bodyStart, contentLength);
        }
    }
    
    parseComplete = true;
    return true;
}

void HttpRequest::reset(){
    method.clear();
    path.clear();
    version.clear();
    headers.clear();
    body.clear();
    parseComplete = false;
}

//HttpResponse 实现
std::string HttpResponse::toString() const{
    std::ostringstream oss;

    oss << "HTTP/1.1 " << statusCode << " " << statusMessage << "\r\n";
    
    for (const auto& [key, value] : headers){
        oss << key << ": " << value << "\r\n";
    }

    oss << "\r\n";
    
    oss << body;
    
    return oss.str();
}

void HttpResponse::setKeepAlive(bool keep){
    if (keep){
        headers["Connection"] = "keep-alive";
        headers["Keep-Alive"] = "timeout=5, max=100";
    } else{
        headers["Connection"] = "close";
    }
}

//ThreadPool 实现
ThreadPool::ThreadPool(int threadNum){
    for (int i = 0; i < threadNum; ++i){
        workers_.emplace_back([this] {
            while (true) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
    LOG_INFO("ThreadPool initialized with %d threads", threadNum);
}

ThreadPool::~ThreadPool(){
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    LOG_INFO("ThreadPool destroyed");
}

void ThreadPool::addTask(Task task){
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

size_t ThreadPool::taskCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tasks_.size();
}


//httpserver实现
HttpServer::HttpServer(int port, int threadNum) : port_(port), pool_(threadNum){
    
    listenFd_.reset(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
    if (listenFd_.get() < 0){
        LOG_ERROR("Failed to create socket");
        throw std::runtime_error("Socket creation failed");
    }
    
    int opt = 1;
    if (setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        LOG_ERROR("Failed to set SO_REUSEADDR");
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listenFd_.get(), (sockaddr*)&addr, sizeof(addr)) < 0){
        LOG_ERROR("Failed to bind port %d", port);
        throw std::runtime_error("Bind failed");
    }
    
    if (listen(listenFd_.get(), SOMAXCONN) < 0){
        LOG_ERROR("Failed to listen");
        throw std::runtime_error("Listen failed");
    }
    
    epollFd_.reset(epoll_create1(0));
    if (epollFd_.get() < 0){
        LOG_ERROR("Failed to create epoll");
        throw std::runtime_error("Epoll creation failed");
    }
    
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listenFd_.get();
    if (epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, listenFd_.get(), &ev) < 0){
        LOG_ERROR("Failed to add listen fd to epoll");
        throw std::runtime_error("Epoll add failed");
    }
    
    LOG_INFO("HttpServer initialized on port %d", port);
}

HttpServer::~HttpServer(){
    running_ = false;
    if (cleanupThread_.joinable()){
        cleanupThread_.join();
    }
    LOG_INFO("HttpServer destroyed");
}

int HttpServer::setNonBlock(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1){
        LOG_ERROR("fcntl F_GETFL failed");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1){
        LOG_ERROR("fcntl F_SETFL failed");
        return -1;
    }
    return 0;
}

void HttpServer::addEpoll(int fd){
    setNonBlock(fd);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0){
        LOG_ERROR("Failed to add fd %d to epoll", fd);
    }
}

void HttpServer::modEpoll(int fd, uint32_t events){
    epoll_event ev{};
    ev.events = events | EPOLLONESHOT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0){
        LOG_ERROR("Failed to modify fd %d in epoll", fd);
    }
}

void HttpServer::delEpoll(int fd){
    epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

void HttpServer::closeConn(int fd){
    std::unique_lock<std::shared_mutex> lock(connMutex_);
    auto it = conns_.find(fd);
    if (it != conns_.end()){
        delEpoll(fd);
        conns_.erase(it);
        LOG_INFO("Connection closed: fd=%d", fd);
    }
}

void HttpServer::run(){
    startCleanupTimer();
    
    std::vector<epoll_event> events(1024);
    LOG_INFO("HttpServer started, listening on port %d", port_);
    
    while (running_){
        int n = epoll_wait(epollFd_.get(), events.data(), events.size(), 1000);
        
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed");
            break;
        }
        
        for (int i = 0; i < n; ++i){
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            
            if (fd == listenFd_.get()){
                handleAccept();
            } else if (ev & (EPOLLERR | EPOLLHUP)) {
                LOG_INFO("Connection error on fd %d", fd);
                closeConn(fd);
            } else if (ev & EPOLLIN){
                handleRead(fd);
            } else if (ev & EPOLLOUT){
                handleWrite(fd);
            }
        }
    }
}

void HttpServer::handleAccept(){
    while (true){
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int cfd = accept4(listenFd_.get(), (sockaddr*)&addr, &len, SOCK_NONBLOCK);
        
        if(cfd < 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("accept failed");
            break;
        }
        
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        LOG_INFO("New connection: fd=%d, ip=%s:%d", cfd, ip, ntohs(addr.sin_port));
        
        addEpoll(cfd);
        
        std::unique_lock<std::shared_mutex> lock(connMutex_);
        conns_[cfd] = std::make_shared<Conn>(cfd);
    }
}

void HttpServer::handleRead(int fd) {
    ConnPtr conn;
    {
        std::shared_lock<std::shared_mutex> lock(connMutex_);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        conn = it->second;
    }
    
    char buf[8192];
    ssize_t n = 0;
    size_t totalRead = 0;
    
    while (true){
        n = read(fd, buf, sizeof(buf) - 1);
        if(n > 0){
            conn->readBuf.append(buf, n);
            totalRead += n;
            conn->lastActive = time(nullptr);
        }else if (n == 0){
            LOG_INFO("Client closed connection: fd=%d", fd);
            closeConn(fd);
            return;
        }else{
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("read error on fd %d", fd);
            closeConn(fd);
            return;
        }
    }
    
    if (totalRead > 0){
        LOG_INFO("Read %zu bytes from fd=%d", totalRead, fd);
        
        if(conn->request.parse(conn->readBuf)) {
            conn->state = ConnState::PROCESSING;
            
            pool_.addTask([this, fd, conn] {
                handleProcess(fd, conn);
            });
        }else{
            modEpoll(fd, EPOLLIN);
        }
    }else{
        modEpoll(fd, EPOLLIN);
    }
}

void HttpServer::handleProcess(int fd, ConnPtr conn){
    processRequest(conn);
    
    prepareResponse(conn);
    
    size_t contentLength = 0;
    auto it = conn->request.headers.find("content-length");
    if(it != conn->request.headers.end()) {
        contentLength = std::stoul(it->second);
    }
    
    size_t headerEnd = conn->readBuf.find("\r\n\r\n");
    size_t totalLength = headerEnd + 4 + contentLength;
    
    if(conn->readBuf.length() > totalLength){
        conn->readBuf = conn->readBuf.substr(totalLength);
    }else{
        conn->readBuf.clear();
    }
    conn->request.reset();
    
    conn->state = ConnState::WRITING;
    modEpoll(fd, EPOLLOUT);
}

void HttpServer::handleWrite(int fd){
    ConnPtr conn;
    {
        std::shared_lock<std::shared_mutex> lock(connMutex_);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        conn = it->second;
    }
    
    if(conn->writeBuf.empty()){
        conn->state = ConnState::READING;
        modEpoll(fd, EPOLLIN);
        return;
    }
    
    ssize_t n = 0;
    size_t totalWritten = 0;
    
    while (!conn->writeBuf.empty()){
        n = write(fd, conn->writeBuf.data(), conn->writeBuf.size());
        if(n > 0){
            conn->writeBuf.erase(0, n);
            totalWritten += n;
            conn->lastActive = time(nullptr);
        }else if(n == 0){
            LOG_INFO("Write returned 0 on fd=%d", fd);
            closeConn(fd);
            return;
        }else{
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                modEpoll(fd, EPOLLOUT);
                break;
            }
            LOG_ERROR("write error on fd %d", fd);
            closeConn(fd);
            return;
        }
    }
    
    if (totalWritten > 0){
        LOG_INFO("Wrote %zu bytes to fd=%d", totalWritten, fd);
    }
    
    if (conn->writeBuf.empty()){
        if (!conn->keepAlive) {
            LOG_INFO("Closing connection (keep-alive: false): fd=%d", fd);
            closeConn(fd);
        }else{
            if (!conn->readBuf.empty() && conn->request.parse(conn->readBuf)){
                pool_.addTask([this, fd, conn] {
                    handleProcess(fd, conn);
                });
            }else{
                conn->state = ConnState::READING;
                modEpoll(fd, EPOLLIN);
            }
        }
    }
}

void HttpServer::processRequest(ConnPtr conn){
    LOG_INFO("Processing request: %s %s", 
             conn->request.method.c_str(), conn->request.path.c_str());
    
    auto it = conn->request.headers.find("connection");
    if (it != conn->request.headers.end()) {
        std::string connHeader = it->second;
        std::transform(connHeader.begin(), connHeader.end(), connHeader.begin(), ::tolower);
        conn->keepAlive = (connHeader == "keep-alive");
    } else {
        conn->keepAlive = (conn->request.version == "HTTP/1.1");
    }
    
    if (conn->request.path == "/" || conn->request.path == "/index.html"){
        conn->response.statusCode = 200;
        conn->response.statusMessage = "OK";
        conn->response.body = 
            "<!DOCTYPE html>\n"
            "<html>\n"
            "<head>\n"
            "    <meta charset=\"UTF-8\">\n"
            "    <title>Mini HttpServer</title>\n"
            "    <style>\n"
            "        body { font-family: Arial, sans-serif; margin: 40px; }\n"
            "        h1 { color: #333; }\n"
            "        .info { background: #f0f0f0; padding: 20px; border-radius: 5px; }\n"
            "    </style>\n"
            "</head>\n"
            "<body>\n"
            "    <h1>🚀 Mini HttpServer</h1>\n"
            "    <div class=\"info\">\n"
            "        <p>Welcome to Mini HttpServer!</p>\n"
            "        <p>This server is built with:</p>\n"
            "        <ul>\n"
            "            <li>Epoll (Edge Triggered)</li>\n"
            "            <li>Non-blocking I/O</li>\n"
            "            <li>Thread Pool</li>\n"
            "            <li>HTTP/1.1 Keep-Alive</li>\n"
            "        </ul>\n"
            "        <p>Request Info:</p>\n"
            "        <ul>\n"
            "            <li>Method: " + conn->request.method + "</li>\n"
            "            <li>Path: " + conn->request.path + "</li>\n"
            "            <li>Version: " + conn->request.version + "</li>\n"
            "        </ul>\n"
            "    </div>\n"
            "</body>\n"
            "</html>";
    } else if (conn->request.path == "/health"){
        conn->response.statusCode = 200;
        conn->response.statusMessage = "OK";
        conn->response.body = "{\"status\":\"ok\",\"connections\":" + 
                             std::to_string(conns_.size()) + "}";
    } else {
        handleNotFound(conn);
    }
}

void HttpServer::prepareResponse(ConnPtr conn){
    conn->response.headers["Content-Type"] = "text/html; charset=utf-8";
    conn->response.headers["Content-Length"] = std::to_string(conn->response.body.size());
    conn->response.headers["Server"] = "Mini-HttpServer/1.0";
    conn->response.setKeepAlive(conn->keepAlive);
    
    conn->writeBuf = conn->response.toString();
    
    LOG_INFO("Prepared response: status=%d, size=%zu, keep-alive=%d",
             conn->response.statusCode, conn->writeBuf.size(), conn->keepAlive);
}

void HttpServer::handleNotFound(ConnPtr conn){
    conn->response.statusCode = 404;
    conn->response.statusMessage = "Not Found";
    conn->response.body = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>404 Not Found</title></head>\n"
        "<body>\n"
        "    <h1>404 Not Found</h1>\n"
        "    <p>The requested URL " + conn->request.path + " was not found on this server.</p>\n"
        "</body>\n"
        "</html>";
}

void HttpServer::handleBadRequest(ConnPtr conn){
    conn->response.statusCode = 400;
    conn->response.statusMessage = "Bad Request";
    conn->response.body = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>400 Bad Request</title></head>\n"
        "<body>\n"
        "    <h1>400 Bad Request</h1>\n"
        "</body>\n"
        "</html>";
}

void HttpServer::startCleanupTimer(){
    cleanupThread_ = std::thread([this]{
        while(running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            cleanupIdleConnections();
        }
    });
}

void HttpServer::cleanupIdleConnections(){
    std::unique_lock<std::shared_mutex> lock(connMutex_);
    time_t now = time(nullptr);
    std::vector<int> toClose;
    
    for(auto& [fd, conn] : conns_){
        if (now - conn->lastActive > 60) { 
            toClose.push_back(fd);
        }
    }
    
    lock.unlock();
    
    for(int fd : toClose){
        LOG_INFO("Closing idle connection: fd=%d", fd);
        closeConn(fd);
    }
}