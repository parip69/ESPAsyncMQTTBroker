#include "AsyncTcpClientAdapter.h"

AsyncTcpClientAdapter::AsyncTcpClientAdapter(AsyncClient* client) :
    _client(client),
    _dataCallback(nullptr), _dataCallbackArg(nullptr),
    _disconnectCallback(nullptr), _disconnectCallbackArg(nullptr),
    _errorCallback(nullptr), _errorCallbackArg(nullptr) {
    if (_client) {
        _client->onData([this](void* arg_unused, AsyncClient* c_unused, void *data, size_t len) {
            this->_handleData(data, len);
        }, nullptr); // arg for AsyncClient onData is not used by us here

        _client->onDisconnect([this](void* arg_unused, AsyncClient* c_unused) {
            this->_handleDisconnect();
        }, nullptr); // arg for AsyncClient onDisconnect is not used by us here

        _client->onError([this](void* arg_unused, AsyncClient* c_unused, int8_t error) {
            this->_handleError(error);
        }, nullptr); // arg for AsyncClient onError is not used by us here

        // Note: Other events like onAck, onPoll, onTimeout could be mapped if needed by ITcpClient
    }
}

AsyncTcpClientAdapter::~AsyncTcpClientAdapter() {
    // The AsyncClient is typically managed by AsyncServer or the user
    // If this adapter were to own the client, it should delete it here.
    // For now, assume external ownership.
    if (_client) {
        // Detach our lambdas to prevent calls on a deleted adapter
        _client->onData(nullptr, nullptr);
        _client->onDisconnect(nullptr, nullptr);
        _client->onError(nullptr, nullptr);
        // _client->close(true); // force close? Or let owner decide?
        // _client = nullptr; // Good practice
    }
}

void AsyncTcpClientAdapter::onData(std::function<void(void *arg, ITcpClient *client, void *data, size_t len)> callback, void *arg) {
    _dataCallback = callback;
    _dataCallbackArg = arg;
}

void AsyncTcpClientAdapter::onDisconnect(std::function<void(void *arg, ITcpClient *client)> callback, void *arg) {
    _disconnectCallback = callback;
    _disconnectCallbackArg = arg;
}

void AsyncTcpClientAdapter::onError(std::function<void(void *arg, ITcpClient *client, int8_t error)> callback, void *arg) {
    _errorCallback = callback;
    _errorCallbackArg = arg;
}

bool AsyncTcpClientAdapter::write(const char *data, size_t len) {
    if (_client) {
        return _client->add(data, len) > 0 && _client->send();
    }
    return false;
}

void AsyncTcpClientAdapter::close() {
    if (_client) {
        _client->close();
    }
}

bool AsyncTcpClientAdapter::connected() {
    return _client ? _client->connected() : false;
}

String AsyncTcpClientAdapter::remoteIP() {
    if (_client) {
        return _client->remoteIP().toString();
    }
    return String();
}

void AsyncTcpClientAdapter::setNoDelay(bool nodelay) {
    if (_client) {
        _client->setNoDelay(nodelay);
    }
}

bool AsyncTcpClientAdapter::canSend() {
    return _client ? _client->canSend() : false;
}

size_t AsyncTcpClientAdapter::space() {
    return _client ? _client->space() : 0;
}

// Internal handlers
void AsyncTcpClientAdapter::_handleData(void *data, size_t len) {
    if (_dataCallback) {
        _dataCallback(_dataCallbackArg, this, data, len);
    }
}

void AsyncTcpClientAdapter::_handleDisconnect() {
    if (_disconnectCallback) {
        _disconnectCallback(_disconnectCallbackArg, this);
    }
}

void AsyncTcpClientAdapter::_handleError(int8_t error) {
    if (_errorCallback) {
        _errorCallback(_errorCallbackArg, this, error);
    }
}
