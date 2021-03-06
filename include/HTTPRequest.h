//
//  HTTPRequest
//

#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <cctype>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#endif

inline int getLastError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

#ifdef _WIN32
inline bool initWSA()
{
    WORD sockVersion = MAKEWORD(2, 2);
    WSADATA wsaData;
    int error = WSAStartup(sockVersion, &wsaData);
    if (error != 0)
    {
        std::cerr << "WSAStartup failed, error: " << error << std::endl;
        return false;
    }

    if (wsaData.wVersion != sockVersion)
    {
        std::cerr << "Incorrect Winsock version" << std::endl;
        WSACleanup();
        return false;
    }

    return true;
}
#endif

namespace http
{
    struct Response
    {
        bool succeeded = false;
        int code = 0;
        std::vector<std::string> headers;
        std::vector<uint8_t> body;
    };

    class Request
    {
    public:
        Request(const std::string& url)
        {
            size_t protocolEndPosition = url.find("://");

            if (protocolEndPosition != std::string::npos)
            {
                protocol = url.substr(0, protocolEndPosition);
                std::transform(protocol.begin(), protocol.end(), protocol.begin(), ::tolower);

                std::string::size_type pathPosition = url.find('/', protocolEndPosition + 3);

                if (pathPosition == std::string::npos)
                {
                    domain = url.substr(protocolEndPosition + 3);
                }
                else
                {
                    domain = url.substr(protocolEndPosition + 3, pathPosition - protocolEndPosition - 3);
                    path = url.substr(pathPosition);
                }

                std::string::size_type portPosition = domain.find(':');

                if (portPosition != std::string::npos)
                {
                    port = domain.substr(portPosition + 1);
                    domain.resize(portPosition);
                }
            }
        }

        ~Request()
        {
            if (socketFd != INVALID_SOCKET)
            {
#ifdef _WIN32
                int result = closesocket(socketFd);
#else
                int result = close(socketFd);
#endif

                if (result < 0)
                {
                    int error = getLastError();
                    std::cerr << "Failed to close socket, error: " << error << std::endl;
                }
            }
        }

        Request(const Request& request) = delete;
        Request(Request&& request) = delete;
        Request& operator=(const Request& request) = delete;
        Request& operator=(Request&& request) = delete;

        Response send(const std::string& method,
                      const std::string& body,
                      const std::vector<std::string>& headers)
        {
            Response response;

            if (protocol != "http")
            {
                std::cerr << "Only HTTP protocol is supported" << std::endl;
                return response;
            }

            if (socketFd != INVALID_SOCKET)
            {
#ifdef _WIN32
                int result = closesocket(socketFd);
#else
                int result = ::close(socketFd);
#endif
                socketFd = INVALID_SOCKET;

                if (result < 0)
                {
                    int error = getLastError();
                    std::cerr << "Failed to close socket, error: " << error << std::endl;
                    return response;
                }
            }

            socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

#ifdef _WIN32
            if (socketFd == INVALID_SOCKET && WSAGetLastError() == WSANOTINITIALISED)
            {
                if (!initWSA()) return response;

                socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            }
#endif

            if (socketFd == INVALID_SOCKET)
            {
                int error = getLastError();
                std::cerr << "Failed to create socket, error: " << error << std::endl;
                return response;
            }

            addrinfo* info;
            if (getaddrinfo(domain.c_str(), port.empty() ? nullptr : port.c_str(), nullptr, &info) != 0)
            {
                int error = getLastError();
                std::cerr << "Failed to get address info of " << domain << ", error: " << error << std::endl;
                return response;
            }

            sockaddr addr = *info->ai_addr;

            freeaddrinfo(info);

            if (::connect(socketFd, &addr, sizeof(addr)) < 0)
            {
                int error = getLastError();

                std::cerr << "Failed to connect to " << domain << ":" << port << ", error: " << error << std::endl;
                return response;
            }
            else
            {
                std::cerr << "Connected to to " << domain << ":" << port << std::endl;
            }

            std::string requestData = method + " " + path + " HTTP/1.1\r\n";

            for (const std::string& header : headers)
            {
                requestData += header + "\r\n";
            }

            requestData += "Host: " + domain + "\r\n";
            requestData += "Content-Length: " + std::to_string(body.size()) + "\r\n";

            requestData += "\r\n";
            requestData += body;

#if defined(__APPLE__)
            int flags = 0;
#elif defined(_WIN32)
            int flags = 0;
#else
            int flags = MSG_NOSIGNAL;
#endif

#ifdef _WIN32
            int remaining = static_cast<int>(requestData.size());
            int sent = 0;
            int size;
#else
            ssize_t remaining = static_cast<ssize_t>(requestData.size());
            ssize_t sent = 0;
            ssize_t size;
#endif

            do
            {
                size = ::send(socketFd, requestData.data() + sent, remaining, flags);

                if (size < 0)
                {
                    int error = getLastError();
                    std::cerr << "Failed to send data to " << domain << ":" << port << ", error: " << error << std::endl;
                    return response;
                }

                remaining -= size;
                sent += size;
            }
            while (remaining > 0);

            uint8_t TEMP_BUFFER[65536];
            const std::vector<uint8_t> clrf = {'\r', '\n'};
            std::vector<uint8_t> responseData;
            bool firstLine = true;
            bool parsedHeaders = false;
            int contentSize = -1;
            bool chunkedResponse = false;
            size_t expectedChunkSize = 0;
            bool removeCLRFAfterChunk = false;

            do
            {
                size = recv(socketFd, reinterpret_cast<char*>(TEMP_BUFFER), sizeof(TEMP_BUFFER), flags);

                if (size < 0)
                {
                    int error = getLastError();
                    std::cerr << "Failed to read data from " << domain << ":" << port << ", error: " << error << std::endl;
                    return response;
                }
                else if (size == 0)
                {
                    // disconnected
                    break;
                }

                responseData.insert(responseData.end(), std::begin(TEMP_BUFFER), std::begin(TEMP_BUFFER) + size);

                if (!parsedHeaders)
                {
                    for (;;)
                    {
                        std::vector<uint8_t>::iterator i = std::search(responseData.begin(), responseData.end(), clrf.begin(), clrf.end());

                        // didn't find a newline
                        if (i == responseData.end()) break;

                        std::string line(responseData.begin(), i);
                        responseData.erase(responseData.begin(), i + 2);

                        // empty line indicates the end of the header section
                        if (line.empty())
                        {
                            parsedHeaders = true;
                            break;
                        }
                        else if (firstLine) // first line
                        {
                            firstLine = false;

                            std::string::size_type pos, lastPos = 0, length = line.length();
                            std::vector<std::string> parts;

                            // tokenize first line
                            while (lastPos < length + 1)
                            {
                                pos = line.find(' ', lastPos);
                                if (pos == std::string::npos) pos = length;

                                if (pos != lastPos)
                                {
                                    parts.push_back(std::string(line.data() + lastPos,
                                                                static_cast<std::vector<std::string>::size_type>(pos) - lastPos));
                                }
                                
                                lastPos = pos + 1;
                            }

                            if (parts.size() >= 2)
                            {
                                response.code = std::stoi(parts[1]);
                            }
                        }
                        else // headers
                        {
                            response.headers.push_back(line);

                            std::string::size_type pos = line.find(':');

                            if (pos != std::string::npos)
                            {
                                std::string headerName = line.substr(0, pos);
                                std::string headerValue = line.substr(pos + 1);

                                // ltrim
                                headerValue.erase(headerValue.begin(),
                                                  std::find_if(headerValue.begin(), headerValue.end(),
                                                               std::not1(std::ptr_fun<int, int>(std::isspace))));

                                // rtrim
                                headerValue.erase(std::find_if(headerValue.rbegin(), headerValue.rend(),
                                                               std::not1(std::ptr_fun<int, int>(std::isspace))).base(),
                                                  headerValue.end());

                                if (headerName == "Content-Length")
                                {
                                    contentSize = std::stoi(headerValue);
                                }
                                else if (headerName == "Transfer-Encoding" && headerValue == "chunked")
                                {
                                    chunkedResponse = true;
                                }
                            }
                        }
                    }
                }

                if (parsedHeaders)
                {
                    if (chunkedResponse)
                    {
                        bool dataReceived = false;
                        for (;;)
                        {
                            if (expectedChunkSize > 0)
                            {
                                auto toWrite = std::min(expectedChunkSize, responseData.size());
                                response.body.insert(response.body.end(), responseData.begin(), responseData.begin() + toWrite);
                                responseData.erase(responseData.begin(), responseData.begin() + toWrite);
                                expectedChunkSize -= toWrite;

                                if (expectedChunkSize == 0) removeCLRFAfterChunk = true;
                                if (responseData.empty()) break;
                            }
                            else
                            {
                                if (removeCLRFAfterChunk)
                                {
                                    if (responseData.size() >= 2)
                                    {
                                        removeCLRFAfterChunk = false;
                                        responseData.erase(responseData.begin(), responseData.begin() + 2);
                                    }
                                    else break;
                                }

                                auto i = std::search(responseData.begin(), responseData.end(), clrf.begin(), clrf.end());

                                if (i == responseData.end()) break;

                                std::string line(responseData.begin(), i);
                                responseData.erase(responseData.begin(), i + 2);

                                expectedChunkSize = std::stoul(line, 0, 16);

                                if (expectedChunkSize == 0)
                                {
                                    dataReceived = true;
                                    break;
                                }
                            }
                        }

                        if (dataReceived)
                        {
                            break;
                        }
                    }
                    else
                    {
                        response.body.insert(response.body.end(), responseData.begin(), responseData.end());
                        responseData.clear();

                        // got the whole content
                        if (contentSize == -1 || response.body.size() >= contentSize)
                        {
                            break;
                        }
                    }
                }
            }
            while (size > 0);

            response.succeeded = true;

            return response;
        }

    private:
        std::string protocol;
        std::string domain;
        std::string port = "80";
        std::string path;
        socket_t socketFd = INVALID_SOCKET;
    };
}
