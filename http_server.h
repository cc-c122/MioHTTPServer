#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <memory>
#include <string>
#include <sstream>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <ctime>

//日志宏
#define LOG_INFO(fmt, ...)\
    do{
        time_t t = time(nullptr);\
        char time_buf[32];\
        strftime(time_buf, sizeof(time_buf), "%y-%m-%d %H:%M:%S", localtime(&t));\
        fprintf(stderr, "[%s] [INFO]" fmt "\n", time_buf, ##__VA_ARGS__);\
    }while(0)

#define LOG_ERROR(fmt, ...)\
    do{
        time_t t = time(nullptr);\
        char time_buf[32];\
        strftime(time_buf, sizeof(time_buf), "%y-%m-%d %H:%M:%S", localtime(&t));\
        fprintf(stderr, "[%s] [ERROR]" fmt ": %s\n", time_buf, ##__VA_ARGS__, strerror(errno));\
    }while(0)

     
//HTTP请求结构
struct HttpRequest{
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool parseComplete = false;

    bool parse(const std::string& raw);
    void reset();
};

//HTTP响应结构
struct HttpResopnse {
    int statusCode = 200;
    std::string statusMessage = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string toSting() const;
    void setKeepAlive(bool keep);
};

//RAII文件描述符封装
class ScopedFd{
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {};
    ~ScopedFd(){ reset(); };

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator = (const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) {
        pther.fd = -1;
    }

    ScopedFd& operator = (ScopedFd&& other) noexcept {
        if(this != &other){
            reset();
            fd_ = other.fd_;
            other.fd = -1; 
        }
        retur *this;
    }

    int get() const {return fd_;};
    void reset(int fd = -1){
        if(fd_ >= 0) close(fd_);
        fd_ = fd;
    }

    int release(){
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

//线程池
class ThreadPool {
public:
    using Task = std::function<void()>;
    explicit ThreadPool(int threadNum = 4);
    ~ThreadPool();
    void addTask(Task task);
    size_t taskCount() const;

private:
    std::vector<std::thread> workers_;
    std::queu<Task> tasks_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

//HTTP服务器主类
class HttpServer {
public:
    HttpServer(int port, int threadNum = 4);
    ~HttpServer();
    void run();

private:
    //状态机
    enum class ConnState{
        READING,
        PROCESSING,
        WRITING,
        CLOSING
    };

    //每个fd对应的数据
    struct Conn{
        int fd;
        std::string readBuf;
        std::string weiteBuf;
        HttpRequest request;
        HttpResopnse response;
        ConnState state =  ConnState::READING;
        time_t lastActive;
        bool keepAlive = false;

        explicit Conn(int f) : fd(f), lastActive(time(nullptr)) {};
    };

    using ConnPtr = std:shared_ptr<Conn>;

    //工具函数
    int setNonBlock(int fd);
    void addEpoll(int fd);
    void modEpoll(int fd, uint32_t events);
    void delEpoll(int fd);

    //事件处理
    void handleAccept();
    void handleRead(int fd);
    void handleWrite(int fd);
    void handleProcess(int fd, ConnPtr conn);
    void closeConn(int fd);

    //业务处理
    void processRequest(ConnPtr conn);
    void prepareResponse(ConnPtr conn);
    void handleStaticFile(ConnPtr conn);
    void handleNotFound(ConnPtr conn);
    void handleBadRequest(ConnPtr conn);

    //定时清理空闲连接
    void startCleanupTimer();
    void cleanupIdleConnections();

    //成员变量
    ScopedFD listenFd_;
    ScopedFD epollFd_;
    ThreadPool pool_;

    std::unordered_map<int, ConnPtr> conns_;
    mutable std::shared_mutex connMutex_;

    int pore_;
    std::atomic<bool> running_{true};
    std::thread cleanupThread_;
};

#endif