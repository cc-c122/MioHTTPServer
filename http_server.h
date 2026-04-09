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


struct HttpRequest{
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool parseComplete = false;

    bool parse(const std::string& raw);
    void reset();
}