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
ThreadPool::ThreadPool(int threadNum) {
    for (int i = 0; i < threadNum; ++i) {
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

ThreadPool::~ThreadPool() {
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

void ThreadPool::addTask(Task task) {
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