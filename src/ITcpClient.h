#ifndef I_TCP_CLIENT_H
#define I_TCP_CLIENT_H

#include <functional>
#include <stddef.h> // For size_t
#include <Arduino.h> // For String, IPAddress if needed, assuming it's available

class ITcpClient {
public:
    virtual ~ITcpClient() {}

    virtual void onData(std::function<void(void *arg, ITcpClient *client, void *data, size_t len)> callback, void *arg) = 0;
    virtual void onDisconnect(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) = 0;
    virtual void onError(std::function<void(void *arg, ITcpClient *client, int8_t error)> callback, void *arg) = 0;
    virtual bool write(const char *data, size_t len) = 0;
    virtual void close() = 0;
    virtual bool connected() = 0;
    virtual String remoteIP() = 0;
    virtual void setNoDelay(bool nodelay) = 0;
    virtual bool canSend() = 0;
    virtual size_t space() = 0;
};

#endif // I_TCP_CLIENT_H
