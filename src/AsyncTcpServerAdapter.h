#ifndef ASYNC_TCP_SERVER_ADAPTER_H
#define ASYNC_TCP_SERVER_ADAPTER_H

#include "ITcpServer.h"
#include "AsyncTcpClientAdapter.h" // Needs to be included before AsyncTCP.h if it uses types from it indirectly
#include <AsyncTCP.h>
#include <functional>
#include <vector> // To store client adapters

class AsyncTcpServerAdapter : public ITcpServer {
public:
    AsyncTcpServerAdapter(uint16_t port);
    ~AsyncTcpServerAdapter() override;

    void onClient(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) override;
    void begin() override;
    void end() override;
    uint16_t port() override;

private:
    AsyncServer* _server;
    uint16_t _port;
    std::function<void(void* arg, ITcpClient* client)> _clientCallback;
    void* _clientCallbackArg;

    // To manage the lifetime of client adapters created
    // std::vector<AsyncTcpClientAdapter*> _clientAdapters; // This might lead to memory issues if not managed carefully

    void _handleClientConnect(AsyncClient* client);
};

#endif // ASYNC_TCP_SERVER_ADAPTER_H
