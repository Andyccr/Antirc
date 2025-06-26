#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    #define SHUT_RDWR SD_BOTH
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netdb.h>
#endif

// 全局变量
std::mutex io_mutex;
std::mutex queue_mutex;
std::condition_variable cv;
std::queue<std::string> message_queue;
std::atomic<bool> running{true};

// 线程安全的控制台输出
void safe_print(const std::string& msg) {
    std::lock_guard<std::mutex> lock(io_mutex);
    std::cout << msg;
    std::cout.flush();
}

void print_help() {
    safe_print("\n=== IRC 客户端命令帮助 ===\n");
    safe_print("/nick <昵称>    - 设置你的昵称\n");
    safe_print("/join <#频道>   - 加入频道\n");
    safe_print("/msg <目标> <消息> - 发送消息\n");
    safe_print("/quit           - 退出客户端\n");
    safe_print("/clear          - 清空屏幕\n");
    safe_print("/help           - 显示此帮助\n");
    safe_print("=========================\n\n");
}

// 清屏函数（跨平台）
void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// 域名解析（跨平台）
bool resolve_address(const char* hostname, sockaddr_in* addr) {
#ifdef _WIN32
    ADDRINFOA hints{};
    ADDRINFOA* result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return false;
    }
    
    if (result) {
        *addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
        freeaddrinfo(result);
        return true;
    }
    return false;
#else
    hostent* host = gethostbyname(hostname);
    if (!host) return false;
    addr->sin_addr = *reinterpret_cast<in_addr*>(host->h_addr);
    return true;
#endif
}

// 接收消息线程
void receive_messages(int sockfd) {
    char buffer[4096];
    while (running) {
        int bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                message_queue.push("[系统] 与服务器的连接已断开\n");
            }
            cv.notify_one();
            running = false;
            break;
        }
        
        buffer[bytes] = '\0';
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            message_queue.push(buffer);
        }
        cv.notify_one();
    }
}

// 显示消息线程
void display_messages() {
    while (running) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        cv.wait(lock, []{ return !message_queue.empty() || !running; });
        
        while (!message_queue.empty()) {
            safe_print(message_queue.front());
            message_queue.pop();
        }
        lock.unlock();
        
        // 显示输入提示（如果正在运行）
        if (running) {
            std::lock_guard<std::mutex> io_lock(io_mutex);
            std::cout << "> " << std::flush;
        }
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup 失败\n";
        return 1;
    }
#endif

    if (argc < 2) {
        safe_print("使用方法: " + std::string(argv[0]) + " <服务器地址> [端口]\n");
        return 1;
    }

    const char* server_address = argv[1];
    int port = (argc > 2) ? atoi(argv[2]) : 6667;

    // 创建套接字
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        safe_print("[系统] 创建套接字失败\n");
        return 1;
    }

    // 解析服务器地址
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_address, &server_addr.sin_addr) != 1) {
        if (!resolve_address(server_address, &server_addr)) {
            safe_print("[系统] 无法解析服务器地址: " + std::string(server_address) + "\n");
            close(sockfd);
            return 1;
        }
    }

    // 连接服务器
    safe_print("[系统] 正在连接到服务器 " + std::string(server_address) + ":" + std::to_string(port) + "...\n");
    if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        safe_print("[系统] 连接服务器失败\n");
#ifdef _WIN32
        safe_print("错误代码: " + std::to_string(WSAGetLastError()) + "\n");
#endif
        close(sockfd);
        return 1;
    }
    safe_print("[系统] 已成功连接服务器！输入/help查看帮助\n");

    // 启动接收和显示线程
    std::thread receiver(receive_messages, sockfd);
    std::thread display(display_messages);
    receiver.detach();
    display.detach();

    // 主输入循环
    std::string input;
    while (running) {
        // 显示输入提示
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            std::cout << "> " << std::flush;
        }
        
        std::getline(std::cin, input);
        if (input.empty()) continue;

        if (input == "/quit") {
            running = false;
            send(sockfd, "QUIT :Goodbye!\r\n", 16, 0);
            break;
        }
        else if (input == "/help") {
            print_help();
        }
        else if (input == "/clear") {
            clear_screen();
        }
        else if (input.find("/nick ") == 0 && input.length() > 6) {
            std::string nick = input.substr(6);
            std::string cmd = "NICK " + nick + "\r\n";
            send(sockfd, cmd.c_str(), cmd.size(), 0);
        }
        else if (input.find("/join ") == 0 && input.length() > 6) {
            std::string channel = input.substr(6);
            if (channel[0] != '#') channel = "#" + channel;
            std::string cmd = "JOIN " + channel + "\r\n";
            send(sockfd, cmd.c_str(), cmd.size(), 0);
        }
        else if (input.find("/msg ") == 0 && input.length() > 5) {
            size_t space_pos = input.find(' ', 5);
            if (space_pos != std::string::npos) {
                std::string target = input.substr(5, space_pos - 5);
                std::string message = input.substr(space_pos + 1);
                std::string cmd = "PRIVMSG " + target + " :" + message + "\r\n";
                send(sockfd, cmd.c_str(), cmd.size(), 0);
            }
            else {
                safe_print("[系统] 格式错误！使用: /msg <目标> <消息>\n");
            }
        }
        else {
            safe_print("[系统] 未知命令。输入/help查看帮助\n");
        }
    }

    // 清理资源
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    
    cv.notify_one();  // 唤醒显示线程
    if (display.joinable()) display.join();
    
#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}