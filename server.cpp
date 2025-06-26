#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <algorithm>  // 添加这个头文件以使用 std::find 和 std::find_if

// 跨平台头文件和库
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
#endif

std::mutex mtx;

struct Client {
    int fd;
    std::string nick;
    std::string channel;
};

std::vector<Client> clients;
std::map<std::string, std::vector<int>> channels;

void broadcast(const std::string& msg, const std::string& channel) {
    std::lock_guard<std::mutex> lock(mtx);
    if (channels.find(channel) != channels.end()) {
        for (int fd : channels[channel]) {
            send(fd, msg.c_str(), msg.size(), 0);
        }
    }
}

void handle_client(int client_fd) {
    char buffer[1024];
    Client client{client_fd, "", ""};

    {
        std::lock_guard<std::mutex> lock(mtx);
        clients.push_back(client);
    }

    while (true) {
        int bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;

        buffer[bytes] = '\0';
        std::string message(buffer);

        if (message.find("NICK ") == 0) {
            client.nick = message.substr(5);
            std::string reply = ":" + client.nick + " NICK :" + client.nick + "\r\n";
            send(client_fd, reply.c_str(), reply.size(), 0);
        } 
        else if (message.find("JOIN ") == 0) {
            client.channel = message.substr(5);
            {
                std::lock_guard<std::mutex> lock(mtx);
                channels[client.channel].push_back(client_fd);
            }
            std::string reply = ":" + client.nick + " JOIN " + client.channel + "\r\n";
            broadcast(reply, client.channel);
        }
        else if (message.find("PRIVMSG ") == 0) {
            size_t pos = message.find(' ', 8);
            std::string target = message.substr(8, pos - 8);
            std::string text = message.substr(pos + 1);
            std::string reply = ":" + client.nick + " PRIVMSG " + target + " :" + text + "\r\n";
            broadcast(reply, client.channel);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = std::find_if(clients.begin(), clients.end(), 
            [client_fd](const Client& c) { return c.fd == client_fd; });
        if (it != clients.end()) clients.erase(it);

        for (auto& [channel, fds] : channels) {
            auto fd_it = std::find(fds.begin(), fds.end(), client_fd);
            if (fd_it != fds.end()) fds.erase(fd_it);
        }
    }
    close(client_fd);
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6667);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind port 6667\n";
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "Failed to listen\n";
        return 1;
    }

    std::cout << "IRC server running on port 6667\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "Failed to accept client\n";
            continue;
        }

        std::thread(handle_client, client_fd).detach();
    }

    close(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}