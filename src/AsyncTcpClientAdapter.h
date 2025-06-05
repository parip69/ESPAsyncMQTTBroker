#ifndef ASYNC_TCP_CLIENT_ADAPTER_H
#define ASYNC_TCP_CLIENT_ADAPTER_H

#include "ITcpClient.h"
#include <AsyncTCP.h>
#include <functional>

class AsyncTcpClientAdapter : public ITcpClient {
public:
    AsyncTcpClientAdapter(AsyncClient* client);
    ~AsyncTcpClientAdapter() override;

    void onData(std::function<void(void *arg, ITcpClient *client, void *data, size_t len)> callback, void *arg) override;
    void onDisconnect(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) override;
    void onError(std::function<void(void *arg, ITcpClient *client, int8_t error)> callback, void *arg) override;
    bool write(const char *data, size_t len) override;
    void close() override;
    bool connected() override;
    String remoteIP() override;
    void setNoDelay(bool nodelay) override;
    bool canSend() override;
    size_t space() override;

private:
    AsyncClient* _client;
    std::function<void(void *arg, ITcpClient *client, void *data, size_t len)> _dataCallback;
    void* _dataCallbackArg;
    std::function<void(void *arg, ITcpClient *client)> _disconnectCallback;
    void* _disconnectCallbackArg;
    std::function<void(void *arg, ITcpClient *client, int8_t error)> _errorCallback;
    void* _errorCallbackArg;

    // Internal AsyncTCP event handlers
    void _handleData(void *data, size_t len);
    void _handleDisconnect();
    void _handleError(int8_t error);
};

#endif // ASYNC_TCP_CLIENT_ADAPTER_H
