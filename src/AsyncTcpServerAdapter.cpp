#include "AsyncTcpServerAdapter.h"

AsyncTcpServerAdapter::AsyncTcpServerAdapter(uint16_t port) :
    _server(new AsyncServer(port)),
    _port(port),
    _clientCallback(nullptr),
    _clientCallbackArg(nullptr) {
}

AsyncTcpServerAdapter::~AsyncTcpServerAdapter() {
    end(); // Ensure server is stopped
    delete _server;
    _server = nullptr;

    // Note: Client adapters created by this server are handed off to the user's callback.
    // The user of ITcpServer (i.e., ESPAsyncMQTTBroker) will be responsible for deleting
    // the ITcpClient* instances it receives. This is a common pattern for factory/callback mechanisms.
    // If we stored them in _clientAdapters vector, we would need to delete them here.
    // However, this adapter's lifetime might be shorter than the clients it creates,
    // or the client might be closed/deleted by the user independently.
}

void AsyncTcpServerAdapter::onClient(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) {
    _clientCallback = callback;
    _clientCallbackArg = arg;

    if (_server) {
        _server->onClient([this](void* arg_unused, AsyncClient* asyncClient) {
            this->_handleClientConnect(asyncClient);
        }, nullptr); // arg for AsyncServer onClient is not used by us here
    }
}

void AsyncTcpServerAdapter::_handleClientConnect(AsyncClient* asyncClient) {
    if (_clientCallback) {
        // Create an adapter for the new AsyncClient
        // The ownership of this new adapter is passed to the callback receiver.
        AsyncTcpClientAdapter* clientAdapter = new AsyncTcpClientAdapter(asyncClient);
        _clientCallback(_clientCallbackArg, clientAdapter);
        // Do NOT delete clientAdapter here. Its lifetime is now managed by the user.
    } else {
        // No callback set, just close the client to prevent dangling resources
        asyncClient->stop();
        // delete asyncClient; // AsyncClient is managed by AsyncServer, no need to delete here
    }
}

void AsyncTcpServerAdapter::begin() {
    if (_server) {
        _server->begin();
    }
}

void AsyncTcpServerAdapter::end() {
    if (_server) {
        _server->end();
        // After server->end(), it should handle disconnecting any active clients.
        // AsyncClient instances are managed by AsyncServer.
    }
}

uint16_t AsyncTcpServerAdapter::port() {
    return _port;
    // Alternatively, if AsyncServer had a port() method:
    // return _server ? _server->port() : 0;
}
