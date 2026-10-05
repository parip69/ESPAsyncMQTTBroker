// @ 2.0.224

#include "ESPAsyncMQTTBroker.h"

#include <cstdarg>
#include <algorithm>

// AsyncTCP kann onDisconnect synchron aufrufen und den MQTTClient freigeben.
// Nach close() darf deshalb kein Zugriff mehr auf diesen Zustand erfolgen.
static void closeMQTTClient(MQTTClient *client)
{
    AsyncClient *transport = client->client;
    client->closing = true;
    client->connected = false;
    // AsyncTCP greift nach onData noch auf den Transport zu. Eigene Transporte
    // erst in onPoll schliessen; dort ist close() die letzte Callback-Aktion.
    if (!client->ownsTransport) transport->close();
}

// Anzahl der Bytes fuer MQTT Remaining Length, ohne temporaere Allokation.
static size_t mqttRemainingLengthBytes(size_t value)
{
    size_t count = 0;
    do { ++count; value /= 128; } while (value);
    return count;
}

// MQTT-1.5.3-1/-2: Wohlgeformtes UTF-8 ohne U+0000, Surrogate oder Overlongs.
// U+FEFF bleibt unverändert (MQTT-1.5.3-3); Binärfelder werden nicht geprüft.
static bool validMQTTUTF8(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len;)
    {
        uint32_t codepoint = data[i++];
        if (codepoint == 0) return false;
        if (codepoint < 0x80) continue;
        size_t trailing;
        uint32_t minimum;
        if (codepoint >= 0xC2 && codepoint <= 0xDF)
        { trailing = 1; minimum = 0x80; codepoint &= 0x1F; }
        else if (codepoint >= 0xE0 && codepoint <= 0xEF)
        { trailing = 2; minimum = 0x800; codepoint &= 0x0F; }
        else if (codepoint >= 0xF0 && codepoint <= 0xF4)
        { trailing = 3; minimum = 0x10000; codepoint &= 0x07; }
        else return false;
        if (trailing > len - i) return false;
        while (trailing--)
        {
            const uint8_t byte = data[i++];
            if ((byte & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    }
    return true;
}

static bool readMQTTField(const uint8_t *data, size_t len, size_t &offset,
                          const uint8_t *&field, size_t &fieldLen)
{
    if (offset > len || len - offset < 2) return false;
    fieldLen = (data[offset] << 8) | data[offset + 1];
    offset += 2;
    if (fieldLen > len - offset) return false;
    field = data + offset;
    offset += fieldLen;
    return true;
}

// Hilfsfunktion für CONNACK + Close

static inline void sendConnackAndClose(MQTTClient *client, uint8_t returnCode)

{

    uint8_t connack[] = {0x20, 0x02, 0x00, returnCode};

    client->client->write((const char *)connack, sizeof(connack));

    closeMQTTClient(client);
}

bool ESPAsyncMQTTBroker::isClientActive(AsyncClient *transport, const MQTTClient *identity) const
{
    const auto it = clients.find(transport);
    return it != clients.end() && it->second.get() == identity &&
           !it->second->closing && transport->connected();
}


// String.concat(ptr,len) liest im ESP32-Core ein zusätzliches Byte.
// Zeichenweise mit vorab reserviertem Speicher vermeiden wir diesen Überlauf.
static String mqttString(const uint8_t *data, size_t length)
{
    String result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) result.concat(static_cast<char>(data[i]));
    return result;
}

static size_t messageBytes(const OutgoingQoSMessage &message)
{
    return sizeof(OutgoingQoSMessage) + message.topic.length() + message.payloadLen;
}

static size_t mqttStorageBudget()
{
    return MQTT_MAX_STORED_BYTES > MQTT_RX_RESERVE_BYTES
        ? MQTT_MAX_STORED_BYTES - MQTT_RX_RESERVE_BYTES : MQTT_MAX_STORED_BYTES / 2;
}

size_t ESPAsyncMQTTBroker::storedBytes() const
{
    size_t bytes = 0;
    auto count = [&bytes](const std::shared_ptr<MQTTClient>& client) {
        bytes += sizeof(MQTTClient) + client->clientId.length() + client->rxBuffer.capacity()
               + client->willTopic.length() + client->willPayloadLen;
        for (const auto &sub : client->subscriptions) bytes += sizeof(Subscription) + sub.filter.length();
        for (const auto &entry : client->outgoingMessages) bytes += messageBytes(entry.second);
        for (const auto &message : client->pendingMessages) bytes += messageBytes(message);
        for (const auto &entry : client->incomingQoS2Messages)
            bytes += sizeof(IncomingQoS2Message) + entry.second.topic.length() + entry.second.payload_len;
    };
    for (const auto &entry : clients) count(entry.second);
    for (const auto &entry : persistentSessions) count(entry.second);
    for (const auto &entry : retainedMessages)
        bytes += sizeof(RetainedMessage) + entry.second->topic.length() + entry.second->length;
    for (const auto &will : pendingWills)
        bytes += sizeof(IncomingQoS2Message) + will.topic.length() + will.payload_len;
    return bytes;
}

void ESPAsyncMQTTBroker::notifyMessage(const String& id, const String& topic,
                                     const uint8_t *payload, size_t length)
{
    const auto textCallback = messageCallback;
    const auto bytesCallback = binaryMessageCallback;
    const String text = textCallback ? mqttString(payload, length) : String();
    if (bytesCallback) bytesCallback(id, topic, payload, length);
    if (textCallback) textCallback(id, topic, text);
}

void ESPAsyncMQTTBroker::disconnectClient(AsyncClient *transport)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    auto it = clients.find(transport);
    if (it == clients.end()) return;
    const auto client = it->second;
    const String id = client->clientId;
    const bool accepted = !id.isEmpty();
    client->closing = true;
    client->connected = false;
    client->client = nullptr;
    transport->onData(nullptr, nullptr);
    transport->onError(nullptr, nullptr);
    transport->onDisconnect(nullptr, nullptr);
    transport->onPoll(nullptr, nullptr);
    clients.erase(it);
    connectedClientsInfo.erase(id);
    std::vector<uint8_t>().swap(client->rxBuffer);
    if (accepted && !client->cleanSession) persistentSessions[id] = client;
    if (client->hasWill && !client->gracefulDisconnect)
    {
        IncomingQoS2Message will;
        will.topic = client->willTopic;
        will.payload = std::move(client->willPayload);
        will.payload_len = client->willPayloadLen;
        will.retained = client->willRetain;
        will.qos = client->willQos;
        pendingWills.push_back(std::move(will));
    }
    client->hasWill = false;
    client->willPayloadLen = 0;
    client->willTopic = "";
    while (!pendingWills.empty())
    {
        const auto &will = pendingWills.front();
        if (!publishMessage(will.topic.c_str(), will.payload.get(), will.payload_len,
                            will.retained, will.qos, "", false)) break;
        pendingWills.pop_front();
    }
    flushMessages();
    const auto callback = clientDisconnectCallback;
    if (accepted && callback) callback(id);
    if (client->ownsTransport) delete transport;
}

bool ESPAsyncMQTTBroker::sendMessage(MQTTClient *client, OutgoingQoSMessage &message, bool duplicate)
{
    AsyncClient *transport = client->client;
    if (!transport || !isClientActive(transport, client) || !client->connected) return false;
    if (message.state == OutgoingQoSState::AwaitingPubcomp)
    {
        uint8_t packet[] = {0x62, 2, static_cast<uint8_t>(message.packetId >> 8), static_cast<uint8_t>(message.packetId)};
        message.sentTime = millis();
        if (transport->write(reinterpret_cast<const char*>(packet), sizeof(packet)) != sizeof(packet))
        {
            if (isClientActive(transport, client)) closeMQTTClient(client);
            return false;
        }
        return true;
    }
    const size_t topicLength = message.topic.length();
    const size_t bodyLength = 2 + topicLength + (message.qos ? 2 : 0) + message.payloadLen;
    const size_t length = 1 + mqttRemainingLengthBytes(bodyLength) + bodyLength;
    std::unique_ptr<uint8_t[]> packet(new (std::nothrow) uint8_t[length]);
    if (!packet) return false; // QoS-State bleibt für späteren Versuch erhalten.
    uint8_t *p = packet.get();
    *p++ = 0x30 | (message.qos << 1) | (message.retain ? 1 : 0) | (message.qos && duplicate ? 8 : 0);
    size_t rest = bodyLength;
    do { uint8_t byte = rest % 128; rest /= 128; *p++ = byte | (rest ? 128 : 0); } while (rest);
    *p++ = topicLength >> 8; *p++ = topicLength;
    memcpy(p, message.topic.c_str(), topicLength); p += topicLength;
    if (message.qos) { *p++ = message.packetId >> 8; *p++ = message.packetId; }
    if (message.payloadLen) memcpy(p, message.payload.get(), message.payloadLen);
    message.transmitted = true;
    message.sentTime = millis();
    if (transport->write(reinterpret_cast<const char*>(packet.get()), length) != length)
    {
        if (isClientActive(transport, client)) closeMQTTClient(client);
        return false;
    }
    return true;
}

void ESPAsyncMQTTBroker::pumpMessages(const std::shared_ptr<MQTTClient>& client)
{
    while (client->connected && !client->closing && !client->pendingMessages.empty())
    {
        if (client->pendingMessages.front().qos && client->outgoingMessages.size() >= MQTT_MAX_INFLIGHT_MESSAGES) return;
        uint16_t id = client->pendingMessages.front().qos ? getNextPacketId(client.get()) : 0;
        if (client->pendingMessages.front().qos && !id) return;
        OutgoingQoSMessage message = std::move(client->pendingMessages.front());
        client->pendingMessages.pop_front();
        if (message.qos)
        {
            message.packetId = id;
            auto inserted = client->outgoingMessages.emplace(id, std::move(message));
            if (!sendMessage(client.get(), inserted.first->second, false)) return;
        }
        else if (!sendMessage(client.get(), message, false)) return;
    }
}

void ESPAsyncMQTTBroker::flushMessages()
{
    std::array<std::shared_ptr<MQTTClient>, MQTT_MAX_CLIENTS> snapshot;
    size_t count = 0;
    for (const auto &entry : clients) if (count < snapshot.size()) snapshot[count++] = entry.second;
    for (size_t i = 0; i < count; ++i) pumpMessages(snapshot[i]);
}

uint16_t ESPAsyncMQTTBroker::getNextPacketId(MQTTClient *client)
{
    for (uint32_t attempt = 0; attempt < 65535; ++attempt)
    {
        const uint16_t id = client->nextPacketId++;
        if (!client->nextPacketId) client->nextPacketId = 1;
        if (id && !client->outgoingMessages.count(id)) return id;
    }
    return 0;
}

// Zentrale Logging-Funktion

void ESPAsyncMQTTBroker::logMessage(DebugLevel level, const char *format, ...)

{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (debugLevel == DEBUG_NONE)
        return; // Keine Ausgabe wenn DEBUG_NONE gesetzt

    if (level <= debugLevel)

    {

        char buffer[256];

        va_list args;

        va_start(args, format);

        vsnprintf(buffer, sizeof(buffer), format, args);

        va_end(args);

        String message = buffer;

        // Log ausgeben (Serial oder über Callback)

        if (level <= DEBUG_ERROR)

        {

            Serial.print("❌ ");
        }

        else if (level <= DEBUG_INFO)

        {

            Serial.print("ℹ️ ");
        }

        else

        {

            Serial.print("🔍 ");
        }

        Serial.println(message);

        // Wenn verfügbar, auch an Callback weiterleiten

        const auto callback = loggingCallback;
        if (callback)

        {

            callback(level, message);
        }
    }
}

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : port(port)

{
#ifdef BROKER_DEBUG_LEVEL
    debugLevel = (DebugLevel)BROKER_DEBUG_LEVEL;
#else
    debugLevel = DEBUG_INFO;
#endif
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker()

{

    stop();

    clients.clear();

    retainedMessages.clear();

    persistentSessions.clear();

    connectedClientsInfo.clear();
}

size_t ESPAsyncMQTTBroker::getConnectedClientCount() const
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    size_t count = 0;
    for (const auto &kv : clients)
    {
        const auto &c = kv.second;
        if (c && c->connected)
        {
            count++;
        }
    }
    return count;
}

void ESPAsyncMQTTBroker::begin()

{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (server) return;
    server.reset(new AsyncServer(port));

    server->onClient([](void *arg, AsyncClient *client)

                     {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        broker->onClient(client, true); }, this);

    server->begin();

    esp_timer_create_args_t timer_args{};

    timer_args.callback = [](void *arg)

    {
        ESPAsyncMQTTBroker *broker = (ESPAsyncMQTTBroker *)arg;

        broker->checkTimeoutsFlag = true; // Nur Flag setzen, Verarbeitung in loop() (BP1-01)
    };

    timer_args.arg = this;

    timer_args.dispatch_method = ESP_TIMER_TASK;

    timer_args.name = "mqtt_timeout_timer";

    esp_timer_create(&timer_args, &timeoutTimer);

    esp_timer_start_periodic(timeoutTimer, 1000000); // 1 second
}

void ESPAsyncMQTTBroker::loop()
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (checkTimeoutsFlag.exchange(false)) checkTimeouts();
}

void ESPAsyncMQTTBroker::stop()
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (timeoutTimer) { esp_timer_stop(timeoutTimer); esp_timer_delete(timeoutTimer); timeoutTimer = nullptr; }
    if (server) { server->end(); server.reset(); }
    checkTimeoutsFlag = false;
    while (!clients.empty())
    {
        const auto client = clients.begin()->second;
        AsyncClient *transport = client->client;
        if (client->ownsTransport) {
            // stop() darf aus onData aufgerufen werden. Aufraeumen ohne
            // Broker-Zugriff im naechsten AsyncTCP-Poll oder Disconnect.
            client->ownsTransport = false;
            disconnectClient(transport);
            transport->onDisconnect([](void*, AsyncClient *socket) { delete socket; }, nullptr);
            transport->onPoll([](void*, AsyncClient *socket) {
                socket->onDisconnect(nullptr, nullptr);
                socket->close();
                delete socket;
            }, nullptr);
        } else closeMQTTClient(client.get());
        // Asynchroner Close darf keinen Callback auf den zerstoerten Broker hinterlassen.
        if (clients.count(transport)) disconnectClient(transport);
    }
}

void ESPAsyncMQTTBroker::checkTimeouts()
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    std::array<std::shared_ptr<MQTTClient>, MQTT_MAX_CLIENTS> snapshot;
    size_t count = 0;
    for (const auto &entry : clients) if (count < snapshot.size()) snapshot[count++] = entry.second;
    const uint32_t now = millis();
    for (size_t i = 0; i < count; ++i)
    {
        const auto &client = snapshot[i];
        if (client->closing) continue;
        if ((!client->connected && now - client->lastActivity > 10000UL) ||
            (client->connected && client->keepAlive && now - client->lastActivity > client->keepAlive * 1500UL))
        {
            closeMQTTClient(client.get()); continue;
        }
        std::array<uint16_t, MQTT_MAX_QUEUED_MESSAGES> ids;
        size_t n = 0;
        for (const auto &entry : client->outgoingMessages) if (n < ids.size()) ids[n++] = entry.first;
        std::sort(ids.begin(), ids.begin() + n, [&client](uint16_t a, uint16_t b) {
            return client->outgoingMessages.at(a).sequence < client->outgoingMessages.at(b).sequence;
        });
        for (size_t j = 0; j < n && client->connected && !client->closing; ++j)
        {
            auto found = client->outgoingMessages.find(ids[j]);
            if (found != client->outgoingMessages.end() && now - found->second.sentTime > 5000UL)
                sendMessage(client.get(), found->second, found->second.transmitted);
        }
        pumpMessages(client);
    }
    while (!pendingWills.empty())
    {
        const auto &will = pendingWills.front();
        if (!publishMessage(will.topic.c_str(), will.payload.get(), will.payload_len,
                            will.retained, will.qos, "", false)) break;
        pendingWills.pop_front();
    }
    flushMessages();
}

void ESPAsyncMQTTBroker::setConfig(const ESPAsyncMQTTBrokerConfig &config)

{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    brokerConfig = config;

    // ---------- AUTH CACHE AUFBAU (einmalig) ----------
    allowedUsersLower.clear();
    authAnonMode = brokerConfig.username.isEmpty();
    authNeedPassword = !brokerConfig.password.isEmpty();

    if (!authAnonMode)
    {
        String list = brokerConfig.username;
        unsigned int start = 0;

        while (start < list.length())
        {
            int comma = list.indexOf(',', start);
            if (comma < 0)
                comma = list.length();

            String u = list.substring(start, comma);
            u.trim();        // entfernt Leerzeichen pro User (z.B. "User 1, User2")
            u.toLowerCase(); // case-insensitiv

            if (!u.isEmpty())
                allowedUsersLower.push_back(u);

            start = comma + 1;
        }
    }

    // WICHTIG: Nicht den debugLevel überschreiben!
    // Der debugLevel wird via BROKER_DEBUG_LEVEL Build-Flag in der platformio.ini gesetzt
    // und sollte NICHT durch setConfig() überschrieben werden.
    // Der Benutzer kann alternativ setDebugLevel() direkt aufrufen, wenn gewünscht.

    logMessage(DEBUG_INFO, "🔧 MQTT-Broker Configuration:");

    logMessage(DEBUG_INFO, "   Username: %s", (brokerConfig.username.isEmpty() ? "[empty]" : brokerConfig.username.c_str()));

    logMessage(DEBUG_INFO, "   Password: %s", (brokerConfig.password.isEmpty() ? "[empty]" : "[set]"));

    logMessage(DEBUG_INFO, "   Auth required: %s", (brokerConfig.username != "" ? "Yes" : "No"));
}

void ESPAsyncMQTTBroker::onClient(AsyncClient *transport, bool ownsTransport)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (clients.size() >= MQTT_MAX_CLIENTS || storedBytes() + sizeof(MQTTClient) > MQTT_MAX_STORED_BYTES)
    {
        transport->close();
        if (ownsTransport) delete transport;
        return;
    }
    auto state = std::make_shared<MQTTClient>();
    state->client = transport;
    state->ownsTransport = ownsTransport;
    state->lastActivity = millis();
    clients[transport] = state;
    transport->onData([](void *arg, AsyncClient *socket, void *bytes, size_t length) {
        auto broker = static_cast<ESPAsyncMQTTBroker*>(arg);
        std::lock_guard<std::recursive_mutex> lock(broker->stateMutex);
        auto it = broker->clients.find(socket);
        if (it == broker->clients.end()) return;
        const auto state = it->second; // Auch bei reentranten Callbacks gültig.
        if (!broker->isClientActive(socket, state.get()) || !length) return;
        const size_t maxBuffered = MQTT_MAX_PACKET_SIZE * 4U;
        const size_t newSize = state->rxBuffer.size() + length;
        const size_t growth = newSize > state->rxBuffer.capacity() ? newSize - state->rxBuffer.capacity() : 0;
        if (length > maxBuffered - state->rxBuffer.size() ||
            growth > MQTT_MAX_STORED_BYTES - std::min<size_t>(broker->storedBytes(), MQTT_MAX_STORED_BYTES))
        {
            closeMQTTClient(state.get()); return;
        }
        const auto input = static_cast<const uint8_t*>(bytes);
        if (growth) state->rxBuffer.reserve(newSize); // Kein unkontrolliertes geometrisches Wachstum.
        state->rxBuffer.insert(state->rxBuffer.end(), input, input + length);
        if (state->processingRx) return;
        state->processingRx = true;
        struct ProcessingGuard {
            MQTTClient *state;
            ~ProcessingGuard() { state->processingRx = false; }
        } processing{state.get()};
        size_t consumed = 0;
        while (state->rxBuffer.size() - consumed >= 2)
        {
            size_t remaining = 0, multiplier = 1, header = 1;
            bool complete = false;
            for (size_t n = 0; n < 4; ++n)
            {
                if (consumed + header >= state->rxBuffer.size()) break;
                const uint8_t byte = state->rxBuffer[consumed + header++];
                remaining += (byte & 127) * multiplier;
                if (!(byte & 128)) { complete = true; break; }
                if (n == 3) { closeMQTTClient(state.get()); return; }
                multiplier *= 128;
            }
            if (!complete) break;
            const size_t packetLength = header + remaining;
            if (packetLength > MQTT_MAX_PACKET_SIZE) { closeMQTTClient(state.get()); return; }
            if (state->rxBuffer.size() - consumed < packetLength) break;
            // Eine Paketkopie hält Nutzdaten auch über Disconnect-/Stop-Callbacks gültig.
            std::vector<uint8_t> packet(state->rxBuffer.begin() + consumed,
                                        state->rxBuffer.begin() + consumed + packetLength);
            broker->processPacket(state.get(), packet.data(), packet.size());
            if (!broker->isClientActive(socket, state.get())) return;
            consumed += packetLength;
        }
        if (consumed) state->rxBuffer.erase(state->rxBuffer.begin(), state->rxBuffer.begin() + consumed);
        // Kleine Puffer wiederverwenden; grosse Pakete hinterlassen keinen dauerhaften RX-Speicher.
        if (state->rxBuffer.empty() && state->rxBuffer.capacity() > 512)
            std::vector<uint8_t>().swap(state->rxBuffer);
    }, this);
    transport->onDisconnect([](void *arg, AsyncClient *socket) {
        static_cast<ESPAsyncMQTTBroker*>(arg)->disconnectClient(socket);
    }, this);
    transport->onPoll([](void *arg, AsyncClient *socket) {
        auto broker = static_cast<ESPAsyncMQTTBroker*>(arg);
        std::lock_guard<std::recursive_mutex> lock(broker->stateMutex);
        auto it = broker->clients.find(socket);
        if (it != broker->clients.end() && it->second->closing) socket->close();
    }, this);
    transport->onError([](void *arg, AsyncClient *socket, int8_t error) {
        auto broker = static_cast<ESPAsyncMQTTBroker*>(arg);
        std::lock_guard<std::recursive_mutex> lock(broker->stateMutex);
        auto it = broker->clients.find(socket);
        if (it == broker->clients.end()) return;
        const auto state = it->second;
        const auto callback = broker->errorCallback;
        if (callback) callback(state->clientId, error, "Client Error");
    }, this);
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "Packet too short for header (len=%d)", len);

        return;
    }

    client->lastActivity = millis();
    uint8_t header = data[0];

    uint8_t packetType = (header >> 4) & 0x0F;

    if (client->closing) return;
    // MQTT-2.2.2-1/-2; MQTT-3.3.1-2/-4: Flags vor jedem Handler prüfen.
    const uint8_t flags = header & 0x0F;
    bool validHeader = false;
    switch (packetType)
    {
    case MQTT_PUBLISH:
    {
        const uint8_t qos = (flags >> 1) & 0x03;
        validHeader = qos != 3 && (qos != 0 || (flags & 0x08) == 0);
        break;
    }
    case MQTT_PUBREL:
    case MQTT_SUBSCRIBE:
    case MQTT_UNSUBSCRIBE:
        validHeader = flags == 0x02;
        break;
    case MQTT_CONNECT:
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBCOMP:
    case MQTT_PINGREQ:
    case MQTT_DISCONNECT:
        validHeader = flags == 0;
        break;
    default:
        // Reservierte Typen und reine Server-Ausgaben sind kein Client-Request.
        break;
    }
    if (!validHeader)
    {
        closeMQTTClient(client);
        return;
    }
    // MQTT-3.1.0-1/-2 und MQTT-3.1.4-5: genau ein CONNECT, danach Annahme nötig.
    if (packetType == MQTT_CONNECT)
    {
        if (client->connectSeen)
        {
            closeMQTTClient(client);
            return;
        }
        client->connectSeen = true;
    }
    else if (!client->connected)
    {
        closeMQTTClient(client);
        return;
    }

    // MQTT 2.2.3: maximal vier Laengenbytes, keine Restbytes im Einzelpaket.
    size_t value = 0, multiplier = 1, idx = 1;
    uint8_t encodedByte = 0;
    do
    {
        if (idx >= len || idx > 4)
        {
            closeMQTTClient(client);
            return;
        }
        encodedByte = data[idx++];
        value += (encodedByte & 0x7F) * multiplier;
        if (idx == 5 && (encodedByte & 0x80))
        {
            closeMQTTClient(client);
            return;
        }
        multiplier *= 128;
    } while (encodedByte & 0x80);
    if (value != len - idx)
    {
        closeMQTTClient(client);
        return;
    }
    // MQTT 2.3.1 und 3.4..3.7: genau zwei ID-Bytes, niemals ID 0.
    if (packetType == MQTT_PUBACK || packetType == MQTT_PUBREC ||
        packetType == MQTT_PUBREL || packetType == MQTT_PUBCOMP)
    {
        if (value != 2 || (data[idx] == 0 && data[idx + 1] == 0))
        {
            closeMQTTClient(client);
            return;
        }
    }
    if ((packetType == MQTT_PINGREQ || packetType == MQTT_DISCONNECT) && value != 0)
    {
        closeMQTTClient(client);
        return;
    }

    switch (packetType)

    {

    case MQTT_CONNECT:

        handleConnect(client, data + idx, value);

        break;

    case MQTT_PUBLISH:

        handlePublish(client, data + idx, value, header);

        break;

    case MQTT_PUBACK:

        handlePuback(client, data + idx, value);

        break;

    case MQTT_SUBSCRIBE:

        handleSubscribe(client, data + idx, value);

        break;

    case MQTT_UNSUBSCRIBE:

        handleUnsubscribe(client, data + idx, value);

        break;

    case MQTT_PINGREQ:

        handlePingReq(client);

        break;

    case MQTT_DISCONNECT:

        handleDisconnect(client);

        break;

    case MQTT_PUBREC:

        handlePubRec(client, data + idx, value);

        break;

    case MQTT_PUBREL:

        handlePubRel(client, data + idx, value);

        break;

    case MQTT_PUBCOMP:

        handlePubComp(client, data + idx, value);

        break;

    default:

        logMessage(DEBUG_DEBUG, "Unknown/unprocessed packet type: %d", packetType);

        break;
    }
}

void ESPAsyncMQTTBroker::handleConnect(MQTTClient *client, uint8_t *data, size_t length)
{
    // MQTT-3.1.4-1: Erst vollständig prüfen; keine Session/Will vor Annahme ändern.
    if (length < 7 || data[0] != 0 || data[1] != 4 || memcmp(data + 2, "MQTT", 4) != 0)
    {
        closeMQTTClient(client); // MQTT-3.1.2-1: falscher Protokollname
        return;
    }
    const uint8_t protocolLevel = data[6];
    if (protocolLevel != MQTT_PROTOCOL_LEVEL)
    {
        sendConnackAndClose(client, 0x01); // MQTT-3.1.2-2: keine fremden Layouts parsen
        return;
    }
    if (length < 10)
    {
        closeMQTTClient(client);
        return;
    }
    const uint8_t flags = data[7];
    const bool cleanSession = (flags & 0x02) != 0;
    const bool willFlag = (flags & 0x04) != 0;
    const uint8_t willQos = (flags >> 3) & 0x03;
    const bool willRetain = (flags & 0x20) != 0;
    const bool passwordFlag = (flags & 0x40) != 0;
    const bool usernameFlag = (flags & 0x80) != 0;
    // MQTT-3.1.2-3/-11/-14/-15/-22: reservierte und widersprüchliche Flags.
    if ((flags & 0x01) || (!willFlag && (willQos != 0 || willRetain)) ||
        (willFlag && willQos == 3) || (passwordFlag && !usernameFlag))
    {
        closeMQTTClient(client);
        return;
    }
    const uint16_t keepAlive = (data[8] << 8) | data[9];
    size_t offset = 10;
    const uint8_t *idData = nullptr, *willTopicData = nullptr, *willData = nullptr;
    const uint8_t *userData = nullptr, *passwordData = nullptr;
    size_t idLen = 0, willTopicLen = 0, willLen = 0, userLen = 0, passwordLen = 0;
    // MQTT-3.1.3-1: vorgeschriebene Reihenfolge, alle Felder innerhalb des Pakets.
    if (!readMQTTField(data, length, offset, idData, idLen) ||
        !validMQTTUTF8(idData, idLen) ||
        (willFlag && (!readMQTTField(data, length, offset, willTopicData, willTopicLen) ||
                      !validMQTTUTF8(willTopicData, willTopicLen) ||
                      !readMQTTField(data, length, offset, willData, willLen))) ||
        (usernameFlag && (!readMQTTField(data, length, offset, userData, userLen) ||
                          !validMQTTUTF8(userData, userLen))) ||
        (passwordFlag && !readMQTTField(data, length, offset, passwordData, passwordLen)) ||
        offset != length)
    {
        closeMQTTClient(client);
        return;
    }
    // Bestehende lokale Grenzen; keine Felder still abschneiden.
    if ((idLen == 0 && !cleanSession) || idLen >= 256)
    {
        sendConnackAndClose(client, 0x02); // MQTT-3.1.3-8/-9
        return;
    }

    if (willFlag && (willTopicLen == 0 || willTopicLen > MQTT_MAX_TOPIC_SIZE ||
                     willLen > MQTT_MAX_PAYLOAD_SIZE))
    {
        closeMQTTClient(client);
        return;
    }
    String clientId, username, password, willTopic;
    if (idLen) clientId = mqttString(idData, idLen);
    if (userLen) username = mqttString(userData, userLen);
    if (passwordLen) password = mqttString(passwordData, passwordLen);
    if (willTopicLen) willTopic = mqttString(willTopicData, willTopicLen);
    AsyncClient *transport = client->client;
    if (willFlag && !isValidPublishTopic(willTopic))
    {
        if (isClientActive(transport, client)) closeMQTTClient(client);
        return;
    }
    if (!isClientActive(transport, client)) return;
    const bool cfgUserSet = !brokerConfig.username.isEmpty();
    const bool cfgPassSet = !brokerConfig.password.isEmpty();
    // --- AUTH-Log im Rahmenformat ---
    if (debugLevel >= DEBUG_INFO)
    {
        String cfgUserStr = brokerConfig.username.isEmpty() ? "<empty>" : brokerConfig.username;
        String cfgPassStr = brokerConfig.password.isEmpty() ? "<empty>" : "<set>";
        String policyMode = (!cfgUserSet) ? "ANON" : (!cfgPassSet ? "USER" : "USER+PASS");
        String tryStr = String(usernameFlag ? "U" : "-") + String(passwordFlag ? "P" : "-");
        String userInStr = usernameFlag ? (username.isEmpty() ? "<none>" : username) : "<none>";
        String passInStr = passwordFlag ? (password.isEmpty() ? "<none>" : "<present>") : "<none>";
        String passLenStr = passwordFlag ? String(password.length()) : "";

        String remoteIpStr = "<unknown>";
        if (client->client)
        {
            IPAddress remoteIp = client->client->remoteIP();
            remoteIpStr = remoteIp.toString();
        }

        String authFrame;
        authFrame.reserve(256);
        authFrame += F("[MQTT][AUTH][BROKER]\n");
        authFrame += F("+------------------------------------------+\n");
        authFrame += F("| clientId : ");
        authFrame += clientId;
        authFrame += F(" |\n");
        authFrame += F("| ip       : ");
        authFrame += remoteIpStr;
        authFrame += F(" |\n");
        authFrame += F("| cfg      : ");
        authFrame += policyMode;
        authFrame += F(" |\n");
        authFrame += F("| try      : ");
        authFrame += tryStr;
        authFrame += F(" |\n");
        authFrame += F("| cfgUser  : ");
        authFrame += cfgUserStr;
        authFrame += F(" |\n");
        authFrame += F("| cfgPass  : ");
        authFrame += cfgPassStr;
        authFrame += F(" |\n");
        authFrame += F("| userIn   : ");
        authFrame += userInStr;
        authFrame += F(" |\n");
        authFrame += F("| passIn   : ");
        authFrame += passInStr;
        authFrame += F(" |\n");
        if (!passLenStr.isEmpty())
        {
            authFrame += F("| passLen  : ");
            authFrame += passLenStr;
            authFrame += F(" |\n");
        }
        authFrame += F("+------------------------------------------+");

        logMessage(DEBUG_INFO, "%s", authFrame.c_str());
        // BP3-01: Zusammenfassung nur bei DEBUG_DEBUG (Auth-Frame oben enthält bereits alle Infos)
        logMessage(DEBUG_DEBUG, "--- MQTT Client Connect Info ---");
        logMessage(DEBUG_DEBUG, "ClientID      : %s", clientId.c_str());
        logMessage(DEBUG_DEBUG, "Username      : '%s' (len=%u)", username.c_str(), (unsigned)username.length());
        logMessage(DEBUG_DEBUG, "Password      : %s (len=%u)", password.isEmpty() ? "<empty>" : "<set>", (unsigned)password.length());
        logMessage(DEBUG_DEBUG, "Flags(usr/pwd): %d / %d", (int)usernameFlag, (int)passwordFlag);
        logMessage(DEBUG_DEBUG, "CleanSession  : %s", cleanSession ? "true" : "false");
        logMessage(DEBUG_DEBUG, "KeepAlive     : %u", (unsigned)keepAlive);
        logMessage(DEBUG_DEBUG, "ProtoVersion  : %u", (unsigned)protocolLevel);
        logMessage(DEBUG_DEBUG, "--------------------------------");
    }
    if (!isClientActive(transport, client)) return;
    const bool authFlagsValid = !cfgUserSet || (usernameFlag && (!cfgPassSet || passwordFlag));
    const bool authenticated = authFlagsValid && authenticateClient(username, password);
    if (!isClientActive(transport, client)) return;
    if (!authenticated)
    {
        sendConnackAndClose(client, 0x04);
        return;
    }
    std::unique_ptr<uint8_t[]> willPayload;
    if (willLen)
    {
        willPayload.reset(new (std::nothrow) uint8_t[willLen]);
        if (!willPayload) { sendConnackAndClose(client, 0x03); return; }
        memcpy(willPayload.get(), willData, willLen);
    }

    if (clientId.isEmpty())
    {
        do { clientId = String("auto-") + String(nextAssignedId++); }
        while (connectedClientsInfo.count(clientId) || persistentSessions.count(clientId));
    }
    size_t sessions = persistentSessions.size();
    for (const auto &entry : clients) if (!entry.second->cleanSession && !entry.second->clientId.isEmpty()) ++sessions;
    bool exists = persistentSessions.count(clientId) != 0;
    for (const auto &entry : clients)
        if (!entry.second->cleanSession && entry.second->clientId == clientId) exists = true;
    if ((!cleanSession && !exists && sessions >= MQTT_MAX_SESSIONS) ||
        storedBytes() + willLen + willTopicLen + idLen > mqttStorageBudget())
    { sendConnackAndClose(client, 0x03); return; }
    std::shared_ptr<MQTTClient> previous;
    for (const auto &entry : clients)
        if (entry.second.get() != client && entry.second->clientId == clientId) { previous = entry.second; break; }
    if (previous)
    {
        AsyncClient *old = previous->client;
        closeMQTTClient(previous.get());
        if (clients.count(old)) disconnectClient(old);
        if (!isClientActive(transport, client)) return;
    }
    auto sessionIt = persistentSessions.find(clientId);
    const bool sessionActuallyRestored = !cleanSession && sessionIt != persistentSessions.end();
    uint8_t connack[] = {0x20, 2, static_cast<uint8_t>(sessionActuallyRestored ? 1 : 0), 0};
    if (transport->write(reinterpret_cast<const char*>(connack), sizeof(connack)) != sizeof(connack))
    { if (isClientActive(transport, client)) closeMQTTClient(client); return; }
    if (!isClientActive(transport, client)) return;
    client->clientId = clientId;
    client->protocolVersion = protocolLevel;
    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;
    if (sessionIt != persistentSessions.end())
    {
        if (sessionActuallyRestored)
        {
            client->subscriptions = std::move(sessionIt->second->subscriptions);
            client->outgoingMessages = std::move(sessionIt->second->outgoingMessages);
            client->pendingMessages = std::move(sessionIt->second->pendingMessages);
            client->incomingQoS2Messages = std::move(sessionIt->second->incomingQoS2Messages);
            client->nextPacketId = sessionIt->second->nextPacketId;
        }
        persistentSessions.erase(sessionIt);
    }
    client->willTopic = willTopic;
    client->willQos = willQos;
    client->willRetain = willRetain;
    client->willPayload = std::move(willPayload);
    client->willPayloadLen = willLen;
    client->connected = true;
    client->hasWill = willFlag; // MQTT-3.1.2-8: nur zur angenommenen Verbindung
    const String ipStr = transport->remoteIP().toString();
    connectedClientsInfo[clientId] = ipStr;
    logMessage(DEBUG_INFO, "[BROKER] CONNECT cid=%s kaSec=%u", clientId.c_str(), keepAlive);
    if (!isClientActive(transport, client)) return;

    if (sessionActuallyRestored)
    {
        std::array<uint16_t, MQTT_MAX_QUEUED_MESSAGES> ids;
        size_t count = 0;
        for (const auto &entry : client->outgoingMessages) if (count < ids.size()) ids[count++] = entry.first;
        std::sort(ids.begin(), ids.begin() + count, [client](uint16_t a, uint16_t b) {
            return client->outgoingMessages.at(a).sequence < client->outgoingMessages.at(b).sequence;
        });
        for (size_t i = 0; i < count; ++i)
        {
            auto message = client->outgoingMessages.find(ids[i]);
            if (message != client->outgoingMessages.end()) sendMessage(client, message->second, message->second.transmitted);
            if (!isClientActive(transport, client)) return;
        }
    }
    pumpMessages(clients.at(transport));
    if (!isClientActive(transport, client)) return;
    const auto callback = clientConnectCallback;
    if (callback)
    {
        callback(clientId, ipStr, username, password.length());
        if (!isClientActive(transport, client)) return;
    }
    // Wiederaufnahme bestehender Subscriptions ist kein neues SUBSCRIBE.
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, size_t length, uint8_t header)
{
    const uint8_t qos = (header >> 1) & 3;
    const bool retained = header & 1;
    if (length < 2) { closeMQTTClient(client); return; }
    const size_t topicLength = (data[0] << 8) | data[1];
    const size_t idLength = qos ? 2 : 0;
    if (!topicLength || topicLength > MQTT_MAX_TOPIC_SIZE || topicLength > length - 2 ||
        length - 2 - topicLength < idLength || !validMQTTUTF8(data + 2, topicLength))
    { closeMQTTClient(client); return; }
    const String topic = mqttString(data + 2, topicLength);
    if (!isValidPublishTopic(topic)) { closeMQTTClient(client); return; }
    size_t offset = 2 + topicLength;
    const uint16_t id = qos ? (data[offset] << 8) | data[offset + 1] : 0;
    if (qos && !id) { closeMQTTClient(client); return; }
    offset += idLength;
    const size_t payloadLength = length - offset;
    AsyncClient *transport = client->client;
    const String source = client->clientId;
    if (qos == 2)
    {
        // Bereits angenommene ID unverändert lassen, unabhängig vom DUP-Bit.
        if (!client->incomingQoS2Messages.count(id))
        {
            if (client->incomingQoS2Messages.size() >= MQTT_MAX_QUEUED_MESSAGES ||
                storedBytes() + sizeof(IncomingQoS2Message) + topicLength + payloadLength > mqttStorageBudget())
            { closeMQTTClient(client); return; }
            IncomingQoS2Message message(topic, data + offset, payloadLength, retained, source);
            if (payloadLength && !message.payload) { closeMQTTClient(client); return; }
            client->incomingQoS2Messages.emplace(id, std::move(message));
        }
        uint8_t pubrec[] = {0x50, 2, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
        if (transport->write(reinterpret_cast<const char*>(pubrec), 4) != 4 && isClientActive(transport, client)) closeMQTTClient(client);
        return;
    }
    // Erst Eigentum für alle passenden Sessions übernehmen, dann bestätigen.
    if (!publishMessage(topic.c_str(), data + offset, payloadLength, retained, qos, "", false))
    { closeMQTTClient(client); return; }
    if (qos == 1)
    {
        uint8_t puback[] = {0x40, 2, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
        if (transport->write(reinterpret_cast<const char*>(puback), 4) != 4 && isClientActive(transport, client)) closeMQTTClient(client);
    }
    flushMessages();
    notifyMessage(source, topic, data + offset, payloadLength);
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, size_t length)
{
    AsyncClient *transport = client->client;
    if (length < 2 || (data[0] == 0 && data[1] == 0))
    {
        closeMQTTClient(client);
        return;
    }
    // MQTT 3.8.3: gesamtes Paket vor Aenderungen validieren.
    size_t offset = 2, filterCount = 0;
    while (offset < length)
    {
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        if (!readMQTTField(data, length, offset, bytes, size) || size == 0 ||
            size > MQTT_MAX_TOPIC_SIZE || !validMQTTUTF8(bytes, size) ||
            offset >= length || data[offset++] > 2)
        {
            closeMQTTClient(client);
            return;
        }
        String filter;
        filter = mqttString(bytes, size);
        const bool valid = isValidTopicFilter(filter);
        if (!isClientActive(transport, client)) return;
        if (!valid)
        {
            closeMQTTClient(client);
            return;
        }
        ++filterCount;
    }
    if (filterCount == 0)
    {
        closeMQTTClient(client);
        return;
    }
    // Genau eine SUBACK-Allokation ohne Filter-/Returncode-Vektor.
    const size_t remaining = 2 + filterCount;
    uint8_t encoded[4];
    size_t encodedSize = 0, rest = remaining;
    do
    {
        uint8_t byte = rest % 128;
        rest /= 128;
        encoded[encodedSize++] = byte | (rest ? 0x80 : 0);
    } while (rest);
    const size_t packetSize = 1 + encodedSize + remaining;
    std::unique_ptr<uint8_t[]> suback(new (std::nothrow) uint8_t[packetSize]);
    if (!suback) { closeMQTTClient(client); return; }
    suback[0] = MQTT_SUBACK << 4;
    memcpy(suback.get() + 1, encoded, encodedSize);
    suback[1 + encodedSize] = data[0];
    suback[2 + encodedSize] = data[1];
    offset = 2;
    size_t codeIndex = 3 + encodedSize;
    size_t reservedRetainedCount = 0, reservedRetainedBytes = 0;
    while (offset < length)
    {
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        readMQTTField(data, length, offset, bytes, size);
        Subscription requested;
        requested.filter = mqttString(bytes, size);
        requested.qos = data[offset++];
        size_t retainedCount = 0, retainedBytes = 0;
        for (const auto &entry : retainedMessages) {
            if (topicMatches(requested, entry.first)) {
                ++retainedCount;
                retainedBytes += sizeof(OutgoingQoSMessage) + entry.first.length() + entry.second->length;
            }
        }
        if (client->pendingMessages.size() + client->outgoingMessages.size() + reservedRetainedCount + retainedCount > MQTT_MAX_QUEUED_MESSAGES ||
            storedBytes() + reservedRetainedBytes + retainedBytes + sizeof(Subscription) + requested.filter.length() > mqttStorageBudget()) {
            suback[codeIndex++] = 0x80; continue;
        }
        bool found = false;
        for (auto &existing : client->subscriptions)
        {
            if (existing.filter == requested.filter)
            {
                existing.qos = requested.qos;
                existing.noLocal = false;
                found = true;
                break;
            }
        }
        if (!found) {
            if (client->subscriptions.size() >= MQTT_MAX_SUBSCRIPTIONS ||
                storedBytes() + sizeof(Subscription) + requested.filter.length() > mqttStorageBudget()) {
                suback[codeIndex++] = 0x80; continue;
            }
            client->subscriptions.push_back(requested);
        }
        suback[codeIndex++] = requested.qos;
        reservedRetainedCount += retainedCount;
        reservedRetainedBytes += retainedBytes;
        auto callback = subscribeCallback;
        if (callback)
        {
            const String id = client->clientId;
            callback(id, requested.filter);
            if (!isClientActive(transport, client)) return;
        }
    }
    if (transport->write((const char *)suback.get(), packetSize) != packetSize)
    {
        closeMQTTClient(client);
        return;
    }
    codeIndex = 3 + encodedSize;
    // MQTT-3.8.4-3/-4: nur angefragte Filter, inklusive Wiederholungen,
    // jeweils mit ihrem QoS wie einzelne SUBSCRIBEs behandeln.
    offset = 2;
    while (offset < length)
    {
        const uint8_t *bytes = nullptr;
        size_t size = 0;
        readMQTTField(data, length, offset, bytes, size);
        Subscription requested;
        requested.filter = mqttString(bytes, size);
        requested.qos = data[offset++];
        if (suback[codeIndex++] != 0x80) sendRetainedMessages(client, requested);
        if (!isClientActive(transport, client)) return;
    }
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, size_t length)
{
    AsyncClient *transport = client->client;
    if (length < 2 || !(data[0] || data[1])) { closeMQTTClient(client); return; }
    size_t offset = 2, count = 0;
    while (offset < length) {
        const uint8_t *bytes; size_t size;
        if (!readMQTTField(data, length, offset, bytes, size) || !isValidTopicFilter(mqttString(bytes, size))) {
            closeMQTTClient(client); return;
        }
        ++count;
    }
    if (!count) { closeMQTTClient(client); return; }
    offset = 2;
    while (offset < length) {
        const uint8_t *bytes; size_t size;
        readMQTTField(data, length, offset, bytes, size);
        const String filter = mqttString(bytes, size);
        bool removed = false;
        for (auto it = client->subscriptions.begin(); it != client->subscriptions.end();) {
            if (it->filter == filter) { it = client->subscriptions.erase(it); removed = true; }
            else ++it;
        }
        auto callback = unsubscribeCallback;
        if (removed && callback) {
            const String id = client->clientId;
            callback(id, filter);
            if (!isClientActive(transport, client)) return;
        }
    }
    const uint8_t ack[] = {0xB0, 2, data[0], data[1]};
    if (transport->write((const char*)ack, sizeof(ack)) != sizeof(ack)) closeMQTTClient(client);
}

void ESPAsyncMQTTBroker::handlePingReq(MQTTClient *client)
{
    uint8_t pingresp[] = {0xD0, 0x00};
    if (client->client->write((const char *)pingresp, 2) != 2) {
        closeMQTTClient(client); return;
    }
    if (!client->kaSeen)
    {
        client->kaSeen = true;
        logMessage(DEBUG_INFO, "[BROKER] KA REGISTERED cid=%s", client->clientId.c_str());
    }
    logMessage(DEBUG_DEBUG, "[BROKER] PINGREQ cid=%s -> PINGRESP", client->clientId.c_str());
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)
{
    client->gracefulDisconnect = true;
    client->hasWill = false;
    client->willPayload.reset();
    client->willPayloadLen = 0;
    // Auch persistente Sessions müssen ihre Netzwerkverbindung schließen.
    closeMQTTClient(client);
}

void ESPAsyncMQTTBroker::handlePuback(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len != 2) return;
    const uint16_t id = (data[0] << 8) | data[1];
    auto it = client->outgoingMessages.find(id);
    if (it != client->outgoingMessages.end() && it->second.state == OutgoingQoSState::AwaitingPuback)
        client->outgoingMessages.erase(it);
    const auto active = clients.find(client->client);
    if (active != clients.end()) pumpMessages(active->second);
}

void ESPAsyncMQTTBroker::handlePubRec(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len != 2) return;
    const uint16_t id = (data[0] << 8) | data[1];
    auto it = client->outgoingMessages.find(id);
    if (it != client->outgoingMessages.end())
    {
        if (it->second.qos != 2) return;
        it->second.state = OutgoingQoSState::AwaitingPubcomp;
        sendMessage(client, it->second, false);
    }
    else
    {
        uint8_t pubrel[] = {0x62, 2, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
        if (client->client->write(reinterpret_cast<const char*>(pubrel), 4) != 4) closeMQTTClient(client);
    }
}

void ESPAsyncMQTTBroker::handlePubRel(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len != 2) return;
    const uint16_t id = (data[0] << 8) | data[1];
    auto it = client->incomingQoS2Messages.find(id);
    IncomingQoS2Message message;
    const bool deliver = it != client->incomingQoS2Messages.end();
    if (deliver)
    {
        if (!publishMessage(it->second.topic.c_str(), it->second.payload.get(), it->second.payload_len,
                            it->second.retained, 2, "", false))
        { closeMQTTClient(client); return; } // Eingangs-State bleibt in persistenter Session.
        message = std::move(it->second);
        client->incomingQoS2Messages.erase(it);
    }
    AsyncClient *transport = client->client;
    uint8_t pubcomp[] = {0x70, 2, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    if (transport->write(reinterpret_cast<const char*>(pubcomp), 4) != 4 && isClientActive(transport, client)) closeMQTTClient(client);
    flushMessages();
    if (deliver) notifyMessage(message.senderClientId, message.topic, message.payload.get(), message.payload_len);
}

void ESPAsyncMQTTBroker::handlePubComp(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len != 2) return;
    const uint16_t id = (data[0] << 8) | data[1];
    auto it = client->outgoingMessages.find(id);
    if (it != client->outgoingMessages.end() && it->second.state == OutgoingQoSState::AwaitingPubcomp)
        client->outgoingMessages.erase(it);
    const auto active = clients.find(client->client);
    if (active != clients.end()) pumpMessages(active->second);
}

bool ESPAsyncMQTTBroker::topicMatches(const Subscription &subscription, const String &topic)

{

    return topicMatches(subscription.filter, topic);
}

bool ESPAsyncMQTTBroker::topicMatches(const String &filter, const String &topic)
{
    // MQTT 4.7: keine Kopien/Allokationen; leere Level sind echte Level.
    const char *f = filter.c_str(), *t = topic.c_str();
    if (!*f || !*t) return false;
    if (*t == '$' && (*f == '#' || *f == '+')) return false;
    for (;;)
    {
        if (f[0] == '#' && f[1] == '\0') return true;
        const char *fe = strchr(f, '/'), *te = strchr(t, '/');
        const size_t fl = fe ? (size_t)(fe - f) : strlen(f);
        const size_t tl = te ? (size_t)(te - t) : strlen(t);
        if (!(fl == 1 && *f == '+') && (fl != tl || memcmp(f, t, fl) != 0))
            return false;
        if (!fe) return !te;
        if (!te) return strcmp(fe, "/#") == 0;
        f = fe + 1;
        t = te + 1;
    }
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client, const Subscription &sub)
{
    // Retained-Daten zuerst in die Session kopieren; Transport-Callbacks erst danach ausfuehren.
    auto active = clients.find(client->client);
    if (active == clients.end()) return;
    auto held = active->second;
    for (const auto &entry : retainedMessages) {
        const auto &retained = *entry.second;
        if (!topicMatches(sub, retained.topic)) continue;
        if (client->pendingMessages.size() + client->outgoingMessages.size() >= MQTT_MAX_QUEUED_MESSAGES ||
            storedBytes() + sizeof(OutgoingQoSMessage) + retained.topic.length() + retained.length > mqttStorageBudget()) {
            closeMQTTClient(client); return;
        }
        OutgoingQoSMessage message;
        message.topic = retained.topic; message.payloadLen = retained.length;
        message.qos = std::min(sub.qos, retained.qos); message.retain = true;
        message.state = message.qos == 2 ? OutgoingQoSState::AwaitingPubrec : OutgoingQoSState::AwaitingPuback;
        if (message.payloadLen) {
            message.payload.reset(new (std::nothrow) uint8_t[message.payloadLen]);
            if (!message.payload) { closeMQTTClient(client); return; }
            memcpy(message.payload.get(), retained.payload.get(), message.payloadLen);
        }
        message.sequence = nextSequence++;
        client->pendingMessages.push_back(std::move(message));
    }
    pumpMessages(held);
}


// BP3-06: isUserAllowed() als toter Code entfernt

bool ESPAsyncMQTTBroker::authenticateClient(const String &username, const String &password)
{
    // ANON: keine Auth konfiguriert -> alles akzeptieren
    if (authAnonMode)
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_INFO, "[AUTH] Mode=ANON: Broker akzeptiert alle anonymen Clients. -> Accept");
        }
        return true;
    }

    // Username normalisieren
    String u = username;
    u.trim();
    u.toLowerCase();

    if (u.isEmpty())
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_ERROR, "[AUTH] Username fehlt/leer -> Reject");
        }
        return false;
    }

    // Username gegen Cache prüfen
    bool userOk = false;
    for (const auto &au : allowedUsersLower)
    {
        if (au == u)
        {
            userOk = true;
            break;
        }
    }

    if (!userOk)
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_ERROR, "[AUTH] Username '%s' nicht in erlaubter Liste -> Reject", u.c_str());
        }
        return false;
    }

    // USER_ONLY
    if (!authNeedPassword)
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_INFO, "[AUTH] Mode=USER_ONLY: Username OK -> Accept");
        }
        return true;
    }

    // USER+PASS
    const String &p = password;

    if (p.isEmpty())
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_ERROR, "[AUTH] Mode=USER_PASS: Passwort fehlt/leer -> Reject");
        }
        return false;
    }

    // Fail-fast: Länge prüfen
    if (p.length() != brokerConfig.password.length())
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_ERROR, "[AUTH] Mode=USER_PASS: Passwort-Länge passt nicht -> Reject");
        }
        return false;
    }

    bool passOk = memcmp(p.c_str(), brokerConfig.password.c_str(), p.length()) == 0;
    if (!passOk)
    {
        if (brokerConfig.log)
        {
            logMessage(DEBUG_ERROR, "[AUTH] Mode=USER_PASS: Passwort falsch -> Reject");
        }
        return false;
    }

    if (brokerConfig.log)
    {
        logMessage(DEBUG_INFO, "[AUTH] Mode=USER_PASS: Username+Pass OK -> Accept");
    }
    return true;
}

bool ESPAsyncMQTTBroker::setPort(uint16_t newPort)

{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);

    if (newPort == 0)

    {

        logMessage(DEBUG_ERROR, "Ungültiger Port 0");

        return false;
    }

    if (server)

    {

        logMessage(DEBUG_WARNING, "Portänderung auf %u abgelehnt – Server läuft", (unsigned)newPort);

        return false;
    }

    port = newPort;

    logMessage(DEBUG_INFO, "Broker-Port gesetzt auf %u (wirksam bei nächstem begin())", (unsigned)newPort);

    return true;
}

bool ESPAsyncMQTTBroker::isValidPublishTopic(const String &topic)
{
    if (topic.isEmpty() || topic.length() > MQTT_MAX_TOPIC_SIZE ||
        !validMQTTUTF8((const uint8_t*)topic.c_str(), topic.length())) return false;
    return topic.indexOf('#') < 0 && topic.indexOf('+') < 0;
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const char *payload, bool retained, uint8_t qos)

{

    return publish(topic, payload, retained, qos, "");
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const char *payload, bool retained, uint8_t qos, const String &excludeClientId)

{

    if (!topic)

    {

        logMessage(DEBUG_ERROR, "Null pointer as topic for C-String Publish");

        return false;
    }

    if (!payload)

    {

        logMessage(DEBUG_DEBUG, "Null pointer as payload for C-String Publish, treating as empty string.");

        return publish(topic, (const uint8_t *)"", 0, retained, qos, excludeClientId);
    }

    return publish(topic, (const uint8_t *)payload, strlen(payload), retained, qos, excludeClientId);
}

bool ESPAsyncMQTTBroker::publish(const char *topic, uint8_t qos, bool retained, const char *payload)

{

    return publish(topic, payload, retained, qos);
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const uint8_t *payload, size_t payloadLen, bool retained, uint8_t qos, const String &excludeClientId)
{
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return publishMessage(topic, payload, payloadLen, retained, qos, excludeClientId, true);
}

bool ESPAsyncMQTTBroker::publishMessage(const char *topic, const uint8_t *payload, size_t payloadLen,
                                      bool retained, uint8_t qos, const String &excludeClientId, bool dispatch)
{
    if (!topic || qos > 2 || (payloadLen && !payload) || payloadLen > MQTT_MAX_PACKET_SIZE) return false;
    const String name(topic);
    if (!isValidPublishTopic(name)) return false;
    const size_t remaining = 2 + name.length() + (qos ? 2 : 0) + payloadLen;
    if (1 + mqttRemainingLengthBytes(remaining) + remaining > MQTT_MAX_PACKET_SIZE) return false;
    struct Target { std::shared_ptr<MQTTClient> client; uint8_t qos; };
    std::array<Target, MQTT_MAX_CLIENTS + MQTT_MAX_SESSIONS> targets;
    size_t count = 0, additional = 0;
    auto collect = [&](const std::shared_ptr<MQTTClient>& client, bool offline) {
        if (client->clientId == excludeClientId || (offline && !qos)) return true;
        if (!offline && (!client->connected || client->closing)) return true;
        int best = -1;
        for (const auto &subscription : client->subscriptions)
            if (topicMatches(subscription, name)) best = std::max(best, (int)subscription.qos);
        if (best < 0) return true;
        if (count == targets.size() || client->pendingMessages.size() + client->outgoingMessages.size() >= MQTT_MAX_QUEUED_MESSAGES) return false;
        targets[count++] = {client, (uint8_t)std::min(best, (int)qos)};
        additional += sizeof(OutgoingQoSMessage) + name.length() + payloadLen;
        return true;
    };
    for (const auto &entry : clients) if (!collect(entry.second, false)) return false;
    for (const auto &entry : persistentSessions) if (!collect(entry.second, true)) return false;
    const auto oldRetained = retainedMessages.find(name);
    size_t bytes = storedBytes();
    if (retained && oldRetained != retainedMessages.end())
        bytes -= sizeof(RetainedMessage) + oldRetained->second->topic.length() + oldRetained->second->length;
    if (retained && payloadLen) {
        if (oldRetained == retainedMessages.end() && retainedMessages.size() >= MQTT_MAX_RETAINED_MESSAGES) return false;
        additional += sizeof(RetainedMessage) + name.length() + payloadLen;
    }
    const size_t budget = mqttStorageBudget();
    if (bytes > budget || additional > budget - bytes) return false;
    // Erst alle Nutzdaten bereitstellen, dann gemeinsam committen; kein Teilversand bei Limitfehlern.
    std::vector<OutgoingQoSMessage> staged;
    staged.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        OutgoingQoSMessage message;
        message.topic = name; message.qos = targets[i].qos;
        message.payloadLen = payloadLen;
        message.state = message.qos == 2 ? OutgoingQoSState::AwaitingPubrec : OutgoingQoSState::AwaitingPuback;
        if (payloadLen) {
            message.payload.reset(new (std::nothrow) uint8_t[payloadLen]);
            if (!message.payload) return false;
            memcpy(message.payload.get(), payload, payloadLen);
        }
        staged.push_back(std::move(message));
    }
    std::unique_ptr<RetainedMessage> saved;
    if (retained && payloadLen) {
        saved.reset(new (std::nothrow) RetainedMessage(name, payload, payloadLen, qos));
        if (!saved || !saved->payload) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        staged[i].sequence = nextSequence++;
        targets[i].client->pendingMessages.push_back(std::move(staged[i]));
    }
    if (retained) {
        if (payloadLen) retainedMessages[name] = std::move(saved);
        else if (oldRetained != retainedMessages.end()) retainedMessages.erase(oldRetained);
    }
    if (dispatch) flushMessages();
    return true;
}


bool ESPAsyncMQTTBroker::isValidTopicFilter(const String &filter)
{
    const size_t length = filter.length();
    if (!length || length > MQTT_MAX_TOPIC_SIZE || !validMQTTUTF8((const uint8_t*)filter.c_str(), length)) return false;
    const char *bytes = filter.c_str();
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == '#' && ((i && bytes[i-1] != '/') || i+1 != length)) return false;
        if (bytes[i] == '+' && ((i && bytes[i-1] != '/') || (i+1 < length && bytes[i+1] != '/'))) return false;
    }
    return true;
}
