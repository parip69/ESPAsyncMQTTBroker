#ifndef I_TCP_SERVER_H
#define I_TCP_SERVER_H

#include <functional>
#include <stdint.h> // For uint16_t
#include "ITcpClient.h" // Needs definition of ITcpClient for the onClient callback

class ITcpServer {
public:
    virtual ~ITcpServer() {}

    virtual void onClient(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) = 0;
    virtual void begin() = 0;
    virtual void end() = 0;
    virtual uint16_t port() = 0;
};

#endif // I_TCP_SERVER_H
