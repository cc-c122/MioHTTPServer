#include"http_server.h"
#include<csignal>
#include<iostream>

std::atomic<bool> running{true};

void signalHandler(int sig){
    if(sig == SIGINT || sig == SIGTERM){
        std::cout << "\nShutting down server..." << std::endl;
        running = false;
    }
}

int main(int argc, char* argv[]){
    int port = 8080;
    int threads = 4;
    
    if(argc > 1){
        port = std::stoi(argv[1]);
    }
    if(argc > 2){
        threads = std::stoi(argv[2]);
    }
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN); 
    
    try{
        HttpServer server(port, threads);
        server.run();
    }catch(const std::exception& e){
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}