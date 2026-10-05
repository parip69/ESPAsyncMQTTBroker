// @ 2.0.222

#include "ESPAsyncMQTTBroker.h"

#include <cstdarg>

// AsyncTCP kann onDisconnect synchron aufrufen und den MQTTClient freigeben.
// Nach close() darf deshalb kein Zugriff mehr auf diesen Zustand erfolgen.
static void closeMQTTClient(MQTTClient *client)
{
    AsyncClient *transport = client->client;
    client->closing = true;
    client->connected = false;
    transport->close();
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

uint16_t ESPAsyncMQTTBroker::getNextPacketId()

{

    if (nextPacketId == 0)

    {

        nextPacketId = 1;
    }

    return nextPacketId++;
}

// Zentrale Logging-Funktion

void ESPAsyncMQTTBroker::logMessage(DebugLevel level, const char *format, ...)

{

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

        if (loggingCallback)

        {

            loggingCallback(level, message);
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

    server.reset(new AsyncServer(port));

    server->onClient([](void *arg, AsyncClient *client)

                     {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        broker->onClient(client); }, this);

    server->begin();

    esp_timer_create_args_t timer_args;

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
    if (checkTimeoutsFlag)
    {
        checkTimeoutsFlag = false;
        checkTimeouts();
    }
}

void ESPAsyncMQTTBroker::stop()

{

    if (timeoutTimer)

    {

        esp_timer_stop(timeoutTimer);

        esp_timer_delete(timeoutTimer);

        timeoutTimer = NULL;
    }

    if (server)

    {

        server->end();

        server.reset();
    }
}

void ESPAsyncMQTTBroker::checkTimeouts()
{
    uint32_t now = millis();
    const uint32_t retryTimeout = 5000; // 5 seconds
    const uint8_t maxRetries = 3;

    for (auto it = clients.begin(); it != clients.end();)
    {
        auto &mqttClient = it->second;

        if (mqttClient->closing)
        {
            ++it;
            continue;
        }

        // Check for client keep-alive timeout
        if (mqttClient->connected && mqttClient->keepAlive > 0 &&
            (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL))
        {
            logMessage(DEBUG_INFO, "Client ⏰ inactive, disconnecting: %s", mqttClient->clientId.c_str());
            MQTTClient *clientToClose = mqttClient.get();
            it++;
            closeMQTTClient(clientToClose);
        }
        else
        {
            // Check for outgoing QoS message timeouts
            for (auto msgIt = mqttClient->outgoingMessages.begin(); msgIt != mqttClient->outgoingMessages.end();)
            {
                auto &outMsg = msgIt->second;
                if (now - outMsg.sentTime > retryTimeout)
                {
                    if (outMsg.retryCount >= maxRetries)
                    {
                        logMessage(DEBUG_ERROR, "QoS %d message for client '%s' (packet ID %u) timed out after %d retries. Discarding.", outMsg.qos, mqttClient->clientId.c_str(), outMsg.packetId, maxRetries);
                        msgIt = mqttClient->outgoingMessages.erase(msgIt);
                    }
                    else
                    {
                        logMessage(DEBUG_INFO, "QoS %d message for client '%s' (packet ID %u) timed out. Retrying (%d/%d)...", outMsg.qos, mqttClient->clientId.c_str(), outMsg.packetId, outMsg.retryCount + 1, maxRetries);
                        outMsg.retryCount++;
                        outMsg.sentTime = now;
                        if (outMsg.state == OutgoingQoSState::AwaitingPuback || outMsg.state == OutgoingQoSState::AwaitingPubrec)
                        {
                            // BP2-01: Resend PUBLISH with DUP flag — Variable-Length-Encoding für Remaining-Length
                            size_t topicLen = outMsg.topic.length();
                            size_t packet_id_len = 2;
                            size_t remainingLength = 2 + topicLen + packet_id_len + outMsg.payloadLen;

                            // Header-Länge berechnen (1 Byte Fixheader + Variable-Length-Bytes)
                            size_t header_len = 1;
                            if (remainingLength <= 127)
                                header_len += 1;
                            else if (remainingLength <= 16383)
                                header_len += 2;
                            else
                                header_len += 3;

                            size_t packetSize = header_len + remainingLength;
                            auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[packetSize]);
                            uint8_t *ptr = packet.get();
                            *ptr++ = (MQTT_PUBLISH << 4) | (outMsg.qos << 1) | (outMsg.retain ? 1 : 0) | 0x08; // Set DUP flag

                            // Variable-Length-Encoding
                            size_t rem_len = remainingLength;
                            do
                            {
                                uint8_t byte = rem_len % 128;
                                rem_len /= 128;
                                if (rem_len > 0)
                                    byte |= 128;
                                *ptr++ = byte;
                            } while (rem_len > 0);

                            *ptr++ = topicLen >> 8;
                            *ptr++ = topicLen & 0xFF;
                            memcpy(ptr, outMsg.topic.c_str(), topicLen);
                            ptr += topicLen;
                            *ptr++ = outMsg.packetId >> 8;
                            *ptr++ = outMsg.packetId & 0xFF;
                            if (outMsg.payloadLen > 0)
                            {
                                memcpy(ptr, outMsg.payload.get(), outMsg.payloadLen);
                            }
                            mqttClient->client->write((const char *)packet.get(), packetSize);
                        }
                        else if (outMsg.state == OutgoingQoSState::AwaitingPubcomp)
                        {
                            // Resend PUBREL
                            uint8_t pubrel[] = {0x62, 0x02, (uint8_t)(outMsg.packetId >> 8), (uint8_t)(outMsg.packetId & 0xFF)};
                            mqttClient->client->write((const char *)pubrel, sizeof(pubrel));
                        }
                        ++msgIt;
                    }
                }
                else
                {
                    ++msgIt;
                }
            }
            ++it;
        }
    }
}

void ESPAsyncMQTTBroker::setConfig(const ESPAsyncMQTTBrokerConfig &config)

{

    brokerConfig = config;

    // ---------- AUTH CACHE AUFBAU (einmalig) ----------
    allowedUsersLower.clear();
    authAnonMode = brokerConfig.username.isEmpty();
    authNeedPassword = !brokerConfig.password.isEmpty();

    if (!authAnonMode)
    {
        String list = brokerConfig.username;
        int start = 0;

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

void ESPAsyncMQTTBroker::onClient(AsyncClient *client)

{

    auto mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());

    mqttClient->client = client;

    mqttClient->connected = false;

    mqttClient->lastActivity = millis();

    mqttClient->keepAlive = 0;

    mqttClient->cleanSession = true;

    mqttClient->hasWill = false;

    mqttClient->gracefulDisconnect = false;

    mqttClient->willQos = 0;

    mqttClient->willRetain = false;

    mqttClient->willPayloadLen = 0;

    mqttClient->kaSeen = false;

    client->onData([](void *arg, AsyncClient *client, void *data, size_t len)

                   {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        auto it = broker->clients.find(client);



        if (it != broker->clients.end()) {



            MQTTClient* mqttClient = it->second.get();

            if (mqttClient->closing || !client->connected()) return;



            const size_t maxBufferedBytes = MQTT_MAX_PACKET_SIZE * 4U;
            if (len == 0 || mqttClient->rxBuffer.size() + len > maxBufferedBytes) {
                broker->logMessage(DEBUG_ERROR, "Receive buffer exceeds limit: %u > %u",
                                   (unsigned)(mqttClient->rxBuffer.size() + len),
                                   (unsigned)maxBufferedBytes);
                mqttClient->rxBuffer.clear();
                closeMQTTClient(mqttClient);
                return;
            }

            const uint8_t *incoming = static_cast<const uint8_t *>(data);
            mqttClient->rxBuffer.insert(mqttClient->rxBuffer.end(), incoming, incoming + len);

            size_t consumed = 0;
            while (mqttClient->rxBuffer.size() - consumed >= 2) {
                size_t remainingLength = 0;
                size_t multiplier = 1;
                size_t headerLength = 1;
                uint8_t encodedByte = 0;
                uint8_t remainingLengthBytes = 0;
                bool headerComplete = false;

                do {
                    if (consumed + headerLength >= mqttClient->rxBuffer.size()) {
                        break;
                    }

                    encodedByte = mqttClient->rxBuffer[consumed + headerLength++];
                    remainingLength += (encodedByte & 0x7F) * multiplier;
                    multiplier *= 128;
                    remainingLengthBytes++;

                    if (remainingLengthBytes > 4) {
                        broker->logMessage(DEBUG_ERROR, "Invalid MQTT Remaining Length");
                        mqttClient->rxBuffer.clear();
                        closeMQTTClient(mqttClient);
                        return;
                    }

                    headerComplete = (encodedByte & 0x80) == 0;
                } while (!headerComplete);

                if (!headerComplete) {
                    break;
                }

                const size_t packetSize = headerLength + remainingLength;
                if (packetSize > MQTT_MAX_PACKET_SIZE) {
                    broker->logMessage(DEBUG_ERROR, "Packet size exceeds limit: %u > %u",
                                       (unsigned)packetSize,
                                       (unsigned)MQTT_MAX_PACKET_SIZE);
                    mqttClient->rxBuffer.clear();
                    closeMQTTClient(mqttClient);
                    return;
                }

                if (mqttClient->rxBuffer.size() - consumed < packetSize) {
                    break;
                }

                broker->processPacket(mqttClient, mqttClient->rxBuffer.data() + consumed, packetSize);
                // Kein Dereferenzieren der alten Identität: close() kann sie zerstört haben.
                auto active = broker->clients.find(client);
                if (active == broker->clients.end() || active->second.get() != mqttClient ||
                    active->second->closing || !client->connected()) return;
                mqttClient = active->second.get();
                consumed += packetSize;
            }

            if (consumed > 0) {
                mqttClient->rxBuffer.erase(mqttClient->rxBuffer.begin(),
                                           mqttClient->rxBuffer.begin() + consumed);
            }



            mqttClient->lastActivity = millis();



        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *client)

                         {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        auto it = broker->clients.find(client);



        if (it != broker->clients.end()) {



            auto& target = it->second;

            target->closing = true;
            target->connected = false;







            if (target->hasWill && !target->gracefulDisconnect) {



                broker->logMessage(DEBUG_INFO, "Unclean disconnect from client %s. Publishing LWT: Topic='%s', QoS=%d, Retain=%s",



                                   target->clientId.c_str(), target->willTopic.c_str(), target->willQos, target->willRetain ? "Yes" : "No");



                broker->publish(target->willTopic.c_str(), target->willPayload.get(), target->willPayloadLen, target->willRetain, target->willQos, "");



                target->hasWill = false;



            } else if (target->hasWill && target->gracefulDisconnect) {



                broker->logMessage(DEBUG_DEBUG, "LWT for client %s not sent (clean disconnect already handled).", target->clientId.c_str());



            }







            // Disconnect-Callback + Aufräumen in beiden Branches (BP2-05)
            String disconnectedClientId = target->clientId; // Vor std::move sichern

            // QoS2-Eingangs-State liegt jetzt im MQTTClient.
            // Bei cleanSession wird er mit dem Client zerstört.
            // Bei persistenter Session bleibt er zusammen mit dem Client erhalten.

            if (!target->cleanSession) {



                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), session will be kept.",



                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");



                broker->persistentSessions[target->clientId] = std::move(target);



            } else {



                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), Clean Session, removing client.",



                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");



            }

            if (broker->clientDisconnectCallback) {
                broker->clientDisconnectCallback(disconnectedClientId);
            }
            broker->connectedClientsInfo.erase(disconnectedClientId);



            broker->clients.erase(it);



        } }, this);

    client->onError([](void *arg, AsyncClient *client, int8_t error)

                    {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        auto it = broker->clients.find(client);



        if (it != broker->clients.end()) {



            MQTTClient* mqttClient = it->second.get();



            if (broker->errorCallback && mqttClient) {



                broker->logMessage(DEBUG_ERROR, "Client %s Error: %d", mqttClient->clientId.c_str(), error);



                broker->errorCallback(mqttClient->clientId, error, "Client Error");



            }



        } }, this);

    clients[client] = std::move(mqttClient);

    logMessage(DEBUG_DEBUG, "New MQTT connection accepted (IP: %s)", client->remoteIP().toString().c_str());
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "Packet too short for header (len=%d)", len);

        return;
    }

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

    size_t multiplier = 1;

    size_t value = 0;

    uint8_t encodedByte;

    size_t idx = 1;

    do

    {

        if (idx >= len)

        {

            logMessage(DEBUG_ERROR, "Packet too short for full Remaining Length");

            return;
        }

        encodedByte = data[idx++];

        value += (encodedByte & 127) * multiplier;

        multiplier *= 128;

        if (multiplier > 128 * 128 * 128)

        {

            logMessage(DEBUG_ERROR, "Remaining Length has invalid format");

            return;
        }

    } while ((encodedByte & 128) != 0);

    if (len < idx + value)

    {

        logMessage(DEBUG_ERROR, "Packet incomplete or damaged");

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
    if (userLen >= 256 || passwordLen >= 256)
    {
        sendConnackAndClose(client, 0x04);
        return;
    }
    if (willFlag && (willTopicLen == 0 || willTopicLen > MQTT_MAX_TOPIC_SIZE ||
                     willLen > MQTT_MAX_PAYLOAD_SIZE))
    {
        closeMQTTClient(client);
        return;
    }
    String clientId, username, password, willTopic;
    if (idLen) clientId.concat((const char *)idData, idLen);
    if (userLen) username.concat((const char *)userData, userLen);
    if (passwordLen) password.concat((const char *)passwordData, passwordLen);
    if (willTopicLen) willTopic.concat((const char *)willTopicData, willTopicLen);
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
        willPayload.reset(new uint8_t[willLen]);
        memcpy(willPayload.get(), willData, willLen);
    }
    auto sessionIt = persistentSessions.find(clientId);
    const bool sessionActuallyRestored = !cleanSession && sessionIt != persistentSessions.end();
    uint8_t connack[] = {0x20, 0x02, (uint8_t)(sessionActuallyRestored ? 0x01 : 0x00), 0x00};
    if (transport->write((const char *)connack, sizeof(connack)) != sizeof(connack))
    {
        closeMQTTClient(client);
        return;
    }
    // Erst jetzt annehmen. Bestehende Session-Wiederherstellung bewusst beibehalten.
    client->clientId = clientId;
    client->protocolVersion = protocolLevel;
    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;
    if (sessionActuallyRestored)
    {
        client->subscriptions = sessionIt->second->subscriptions;
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
    if (clientConnectCallback)
    {
        clientConnectCallback(clientId, ipStr, username, password.length());
        if (!isClientActive(transport, client)) return;
    }
    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, size_t length, uint8_t header)

{

    uint8_t qos = (header & 0x06) >> 1;

    bool retained = (header & 0x01) != 0;

    if (length < 2)

    {

        logMessage(DEBUG_ERROR, "Publish packet too short");

        return;
    }

    uint16_t topicLength = (data[0] << 8) | data[1];

    if (2 + topicLength > length)

    {

        logMessage(DEBUG_ERROR, "Publish packet too short for topic");

        return;
    }

    if (topicLength > MQTT_MAX_TOPIC_SIZE)

    {

        logMessage(DEBUG_ERROR, "Topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);

        return;
    }

    // Topic direkt als String ohne 257-Byte Stack-Buffer konstruieren (BP1-03)
    String topic;
    topic.concat((const char *)(data + 2), topicLength);

    if (!isValidPublishTopic(topic))

    {

        logMessage(DEBUG_ERROR, "Invalid Topic Name '%s' from client '%s'. Closing connection.", topic.c_str(), client->clientId.c_str());

        if (client->client)

        {

            closeMQTTClient(client);
        }

        return;
    }

    size_t payloadOffset = 2 + topicLength;

    uint16_t packetId = 0;

    if (qos > 0)

    {

        if (payloadOffset + 2 > length)

        {

            logMessage(DEBUG_ERROR, "Publish packet too short for QoS Packet-ID");

            return;
        }

        packetId = (data[payloadOffset] << 8) | data[payloadOffset + 1];

        payloadOffset += 2;

        if (qos == 1)

        {

            uint8_t puback[] = {0x40, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};

            client->client->write((const char *)puback, 4);
        }

        else if (qos == 2)

        {

            uint32_t payloadLength = length - payloadOffset;

            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)

            {

                logMessage(DEBUG_WARNING, "QoS 2 Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);

                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            IncomingQoS2Message qos2Msg(topic, data + payloadOffset, payloadLength, retained, client->clientId);

            // packetId ist nur pro Verbindung eindeutig, daher Ablage pro Client
            client->incomingQoS2Messages[packetId] = std::move(qos2Msg);

            logMessage(DEBUG_INFO, "QoS 2 Publish received - Topic='%s', PacketID=%u. Sending PUBREC.", topic.c_str(), packetId);

            uint8_t pubrec[] = {(MQTT_PUBREC << 4), 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};

            client->client->write((const char *)pubrec, 4);

            return;
        }
    }

    // Verteilung der Nachricht an Abonnenten für QoS 0 und QoS 1.

    // (QoS 2 wird erst nach Abschluss des Handshakes in handlePubRel verteilt.)

    if (qos == 0 || qos == 1)

    {

        uint32_t payloadLength = length - payloadOffset;

        if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)

        {

            logMessage(DEBUG_WARNING, "Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);

            payloadLength = MQTT_MAX_PAYLOAD_SIZE;
        }

        // Payload direkt als String konstruieren.
        // Auch leere MQTT-Payloads werden korrekt weitergeleitet.
        String originalPayload;

        if (payloadLength > 0)

        {

            originalPayload.concat((const char *)(data + payloadOffset), payloadLength);
        }

        // Normales INFO-Log bleibt kompakt.
        logMessage(
            DEBUG_INFO,
            "🔔 Weiterleiten (QoS %d, von %s) - Topic='%s', PayloadLen=%u, Retained=%s",
            qos,
            client->clientId.c_str(),
            topic.c_str(),
            (unsigned)payloadLength,
            retained ? "Yes" : "No"
        );

        // Vollständige Nutzlast nur im ausführlichen Debug-Level.
        logMessage(
            DEBUG_DEBUG,
            "Payload='%s'",
            originalPayload.c_str()
        );

        publish(topic.c_str(), originalPayload.c_str(), retained, qos, client->clientId);

        if (messageCallback)

        {

            messageCallback(client->clientId, topic, originalPayload);
        }

    }
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, size_t length)

{

    // MQTT-2.3.1-1, MQTT-3.8.3-1/-3 und MQTT-3-8.3-4:
    // Erst das gesamte Paket validieren, bevor ein einziger Filter übernommen wird.
    AsyncClient *transport = client->client;
    if (length < 2 || (data[0] == 0 && data[1] == 0))
    {
        closeMQTTClient(client);
        return;
    }
    size_t validationOffset = 2;
    size_t filterCount = 0;
    while (validationOffset < length)
    {
        const uint8_t *filterData = nullptr;
        size_t filterLen = 0;
        if (!readMQTTField(data, length, validationOffset, filterData, filterLen) ||
            filterLen == 0 || filterLen > MQTT_MAX_TOPIC_SIZE ||
            !validMQTTUTF8(filterData, filterLen) || validationOffset >= length ||
            data[validationOffset++] > 2)
        {
            closeMQTTClient(client);
            return;
        }
        String filter;
        filter.concat((const char *)filterData, filterLen);
        const bool validFilter = isValidTopicFilter(filter);
        if (!isClientActive(transport, client)) return;
        if (!validFilter)
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

    if (length < 2)

    {

        logMessage(DEBUG_ERROR, "Subscribe packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    size_t index = 2;

    std::vector<uint8_t> returnCodes;

    while (index < length)

    {

        if (index + 2 > length)

            break;

        uint16_t topicLength = (data[index] << 8) | data[index + 1];

        index += 2;

        if (index + topicLength > length)

            break;

        if (topicLength > MQTT_MAX_TOPIC_SIZE)

        {

            logMessage(DEBUG_ERROR, "Subscribe topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);

            break;
        }

        char topicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};

        memcpy(topicBuffer, data + index, topicLength);
        topicBuffer[topicLength] = '\0';

        String topic = String(topicBuffer);

        index += topicLength;

        if (index >= length)

            break;

        uint8_t options = data[index++];

        uint8_t requestedQoS = options & 0x03;

        const bool noLocal = false; // MQTT 3.1.1: obere sechs Bits wurden bereits abgewiesen.

        logMessage(DEBUG_DEBUG, "Subscribe: Topic '%s', QoS %d, noLocal: %s", topicBuffer, requestedQoS, noLocal ? "true" : "false");
        if (!isClientActive(transport, client)) return;

        if (isValidTopicFilter(topic))

        {

            // BP2-04: Doppelte Subscriptions verhindern — MQTT-Spec erlaubt Update, nicht doppeln
            bool found = false;
            for (auto &existing : client->subscriptions)
            {
                if (existing.filter == topic)
                {
                    existing.qos = requestedQoS;
                    existing.noLocal = noLocal;
                    found = true;
                    logMessage(DEBUG_DEBUG, "Subscription for client '%s' to topic '%s' updated (QoS %d, noLocal %s).",
                               client->clientId.c_str(), topic.c_str(), requestedQoS, noLocal ? "Yes" : "No");
                    if (!isClientActive(transport, client)) return;
                    break;
                }
            }
            if (!found)
            {
                Subscription sub;
                sub.filter = topic;
                sub.qos = requestedQoS;
                sub.noLocal = noLocal;
                client->subscriptions.push_back(sub);
            }

            returnCodes.push_back(requestedQoS);

            logMessage(DEBUG_INFO, "Subscription for client '%s' to topic filter '%s' added (QoS %d, noLocal %s).", client->clientId.c_str(), topic.c_str(), requestedQoS, noLocal ? "Yes" : "No");
            if (!isClientActive(transport, client)) return;

            if (subscribeCallback)

            {

                const String callbackClientId = client->clientId;
                subscribeCallback(callbackClientId, topic);
                if (!isClientActive(transport, client)) return;
            }
        }

        else

        {

            logMessage(DEBUG_WARNING, "Subscription for client '%s' to invalid topic filter '%s' rejected.", client->clientId.c_str(), topic.c_str());

            if (client->protocolVersion == MQTT_PROTOCOL_LEVEL_5)

            {

                returnCodes.push_back(0x8F);
            }

            else

            {

                returnCodes.push_back(0x80);
            }
        }
    }

    if (returnCodes.empty())

    {

        logMessage(DEBUG_ERROR, "No valid subscriptions in SUBSCRIBE packet");

        return;
    }

    size_t subackLength = 2 + returnCodes.size();

    std::unique_ptr<uint8_t[]> suback(new uint8_t[2 + subackLength]);

    suback[0] = MQTT_SUBACK << 4;

    suback[1] = subackLength;

    suback[2] = packetId >> 8;

    suback[3] = packetId & 0xFF;

    for (size_t i = 0; i < returnCodes.size(); i++)

    {

        suback[4 + i] = returnCodes[i];
    }

    client->client->write((const char *)suback.get(), 2 + subackLength);

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, size_t length)

{

    AsyncClient *transport = client->client;

    if (length < 2)

    {

        logMessage(DEBUG_ERROR, "Unsubscribe packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    uint8_t unsuback[4] = {0xB0, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};

    client->client->write((const char *)unsuback, 4);

    size_t index = 2;

    while (index < length)

    {

        if (index + 2 > length)

            break;

        uint16_t topicLength = (data[index] << 8) | data[index + 1];

        index += 2;

        if (index + topicLength > length)

            break;

        if (topicLength > MQTT_MAX_TOPIC_SIZE)

        {

            logMessage(DEBUG_ERROR, "Unsubscribe topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);

            break;
        }

        char topicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};

        memcpy(topicBuffer, data + index, topicLength);
        topicBuffer[topicLength] = '\0';

        String topic = String(topicBuffer);

        index += topicLength;

        for (auto it = client->subscriptions.begin(); it != client->subscriptions.end();)

        {

            if (it->filter == topic)

            {

                if (unsubscribeCallback)

                {

                    const String callbackClientId = client->clientId;
                    unsubscribeCallback(callbackClientId, topic);
                    if (!isClientActive(transport, client)) return;
                }

                it = client->subscriptions.erase(it);
            }

            else

            {

                ++it;
            }
        }
    }
}

void ESPAsyncMQTTBroker::handlePingReq(MQTTClient *client)
{
    uint8_t pingresp[] = {0xD0, 0x00};
    client->client->write((const char *)pingresp, 2);
    if (!client->kaSeen)
    {
        client->kaSeen = true;
        logMessage(DEBUG_INFO, "[BROKER] KA REGISTERED cid=%s", client->clientId.c_str());
    }
    logMessage(DEBUG_DEBUG, "[BROKER] PINGREQ cid=%s -> PINGRESP", client->clientId.c_str());
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)

{

    logMessage(DEBUG_INFO, "Clean disconnect from client %s (DISCONNECT packet received).", client->clientId.c_str());

    client->connected = false;

    client->gracefulDisconnect = true;

    if (client->hasWill)

    {

        logMessage(DEBUG_DEBUG, "LWT for client %s is discarded (clean disconnect).", client->clientId.c_str());

        client->hasWill = false;

        client->willTopic = "";

        client->willPayload.reset();

        client->willPayloadLen = 0;
    }

    if (client->cleanSession && client->client)

    {

        closeMQTTClient(client);
    }
}

void ESPAsyncMQTTBroker::handlePuback(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "Puback packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    auto it = client->outgoingMessages.find(packetId);

    if (it != client->outgoingMessages.end())

    {

        if (it->second.qos == 1)

        {

            logMessage(DEBUG_DEBUG, "PUBACK from subscriber '%s' for packet ID %u received.", client->clientId.c_str(), packetId);

            client->outgoingMessages.erase(it);
        }

        else

        {

            logMessage(DEBUG_WARNING, "Received PUBACK for QoS 2 message from '%s' (packet ID %u). This is unexpected.", client->clientId.c_str(), packetId);
        }
    }

    else

    {

        logMessage(DEBUG_DEBUG, "Spurious PUBACK from '%s' for packet ID %u received.", client->clientId.c_str(), packetId);
    }
}

void ESPAsyncMQTTBroker::handlePubRec(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "PubRec packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    // Check if this is a PUBREC from a subscriber

    auto it = client->outgoingMessages.find(packetId);

    if (it != client->outgoingMessages.end() && it->second.state == OutgoingQoSState::AwaitingPubrec)

    {

        logMessage(DEBUG_DEBUG, "PUBREC from subscriber '%s' for packet ID %u received.", client->clientId.c_str(), packetId);

        // Update state and send PUBREL

        it->second.state = OutgoingQoSState::AwaitingPubcomp;

        it->second.sentTime = millis();

        uint8_t pubrel[] = {0x62, 0x02, (uint8_t)(packetId >> 8), (uint8_t)(packetId & 0xFF)};

        client->client->write((const char *)pubrel, sizeof(pubrel));

        logMessage(DEBUG_DEBUG, "Sending PUBREL to subscriber '%s' for packet ID %u.", client->clientId.c_str(), packetId);

        return;
    }

    // Original logic for PUBREC from a publisher

    uint8_t pubrel[] = {0x62, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};

    client->client->write((const char *)pubrel, sizeof(pubrel));

    logMessage(DEBUG_DEBUG, "PUBREC for publisher packet ID %u processed", packetId);
}

void ESPAsyncMQTTBroker::handlePubRel(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "PubRel packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    auto it = client->incomingQoS2Messages.find(packetId);

    if (it != client->incomingQoS2Messages.end())

    {

        IncomingQoS2Message &msg = it->second;

        String payloadStr;

        if (msg.payload_len > 0 && msg.payload)

        {

            // BP3-02: VLA durch Heap-Allokation ersetzt (VLA ist kein Standard-C++)
            std::unique_ptr<char[]> tempPayload(new char[msg.payload_len + 1]);
            memcpy(tempPayload.get(), msg.payload.get(), msg.payload_len);
            tempPayload[msg.payload_len] = '\0';
            payloadStr = String(tempPayload.get());
        }

        else

        {

            payloadStr = "";
        }

        logMessage(DEBUG_INFO, "PUBREL for packet ID %u received. Publishing QoS 2 message: Topic='%s'", packetId, msg.topic.c_str());

        publish(msg.topic.c_str(), payloadStr.c_str(), msg.retained, MQTT_QOS2, msg.originalClientId);

        client->incomingQoS2Messages.erase(it);
    }

    else

    {

        logMessage(DEBUG_WARNING, "PUBREL for unknown packet ID %u received.", packetId);
    }

    uint8_t pubcomp[] = {(MQTT_PUBCOMP << 4), 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};

    client->client->write((const char *)pubcomp, sizeof(pubcomp));

    logMessage(DEBUG_DEBUG, "PUBCOMP for packet ID %u sent.", packetId);
}

void ESPAsyncMQTTBroker::handlePubComp(MQTTClient *client, uint8_t *data, size_t len)

{

    if (len < 2)

    {

        logMessage(DEBUG_ERROR, "PubComp packet too short");

        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    // Check if this is a PUBCOMP from a subscriber

    auto it = client->outgoingMessages.find(packetId);

    if (it != client->outgoingMessages.end() && it->second.state == OutgoingQoSState::AwaitingPubcomp)

    {

        logMessage(DEBUG_DEBUG, "PUBCOMP from subscriber '%s' for packet ID %u received. QoS 2 flow complete.", client->clientId.c_str(), packetId);

        client->outgoingMessages.erase(it);

        return;
    }

    // Original logic for PUBCOMP from a publisher

    logMessage(DEBUG_DEBUG, "PUBCOMP for publisher packet ID %u received", packetId);
}

bool ESPAsyncMQTTBroker::topicMatches(const Subscription &subscription, const String &topic)

{

    return topicMatches(subscription.filter, topic);
}

bool ESPAsyncMQTTBroker::topicMatches(const String &filter, const String &topic)

{

    const char *f = filter.c_str();

    const char *t = topic.c_str();

    while (*f && *t)

    {

        const char *f_end = strchr(f, '/');

        const char *t_end = strchr(t, '/');

        size_t f_len = f_end ? (size_t)(f_end - f) : strlen(f);

        if (f_len == 1 && *f == '#')

        {

            return true;
        }

        if (f_len == 1 && *f == '+')

        {

            f = f_end ? f_end + 1 : f + f_len;

            t = t_end ? t_end + 1 : t + strlen(t);

            continue;
        }

        size_t t_len = t_end ? (size_t)(t_end - t) : strlen(t);

        if (f_len != t_len || strncmp(f, t, f_len) != 0)

        {

            return false;
        }

        f = f_end ? f_end + 1 : f + f_len;

        t = t_end ? t_end + 1 : t + t_len;
    }

    if (*f && strcmp(f, "/#") == 0)

    {

        return true;
    }

    return *f == *t;
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client)

{

    for (auto const &entry : retainedMessages)

    {

        auto const &msg = entry.second;

        if (!msg)

        {

            logMessage(DEBUG_ERROR, "Error: Invalid unique_ptr in retainedMessages Map found.");

            continue;
        }

        for (auto &sub : client->subscriptions)

        {

            if (topicMatches(sub, msg->topic))

            {

                size_t topicLength = msg->topic.length();

                if (topicLength > MQTT_MAX_TOPIC_SIZE)

                {

                    logMessage(DEBUG_ERROR, "Retained Topic too long: %u > %u", (unsigned)topicLength, MQTT_MAX_TOPIC_SIZE);

                    continue;
                }

                size_t actualPayloadLength = msg->length;

                if (msg->length > MQTT_MAX_PAYLOAD_SIZE)

                {

                    logMessage(DEBUG_WARNING, "Retained Payload for Topic '%s' will be truncated: %u > %u", msg->topic.c_str(), (unsigned)msg->length, MQTT_MAX_PAYLOAD_SIZE);

                    actualPayloadLength = MQTT_MAX_PAYLOAD_SIZE;
                }

                // MQTT: Zustellung maximal mit dem QoS der Subscription.
                uint8_t final_qos = (msg->qos < sub.qos) ? msg->qos : sub.qos;
                size_t packet_id_len = (final_qos > 0) ? 2 : 0;
                size_t remainingLengthField = 2 + topicLength + packet_id_len + actualPayloadLength;

                // BP2-02: Variable-Length-Encoding statt 1-Byte-Limit
                size_t header_len = 1; // Fixed header byte
                if (remainingLengthField <= 127)
                    header_len += 1;
                else if (remainingLengthField <= 16383)
                    header_len += 2;
                else
                    header_len += 3;

                size_t totalPacketLength = header_len + remainingLengthField;

                if (totalPacketLength > MQTT_MAX_PACKET_SIZE)

                {

                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) exceeds MQTT_MAX_PACKET_SIZE: %u > %u.", msg->topic.c_str(), (unsigned)totalPacketLength, MQTT_MAX_PACKET_SIZE);

                    continue;
                }

                std::unique_ptr<uint8_t[]> packet(new uint8_t[totalPacketLength]);

                uint8_t *ptr = packet.get();
                *ptr++ = (MQTT_PUBLISH << 4) | (final_qos << 1) | 0x01;

                // Variable-Length-Encoding
                size_t rem_len = remainingLengthField;
                do
                {
                    uint8_t byte = rem_len % 128;
                    rem_len /= 128;
                    if (rem_len > 0)
                        byte |= 128;
                    *ptr++ = byte;
                } while (rem_len > 0);

                *ptr++ = topicLength >> 8;
                *ptr++ = topicLength & 0xFF;

                memcpy(ptr, msg->topic.c_str(), topicLength);
                ptr += topicLength;

                if (final_qos > 0)
                {
                    uint16_t packetId = getNextPacketId();
                    *ptr++ = packetId >> 8;
                    *ptr++ = packetId & 0xFF;

                    auto outMsg = std::make_unique<OutgoingQoSMessage>();
                    outMsg->qos = final_qos;
                    outMsg->retain = true;
                    outMsg->topic = msg->topic;
                    outMsg->payloadLen = actualPayloadLength;

                    if (actualPayloadLength > 0 && msg->payload)
                    {
                        outMsg->payload = std::unique_ptr<uint8_t[]>(new uint8_t[actualPayloadLength]);
                        memcpy(outMsg->payload.get(), msg->payload.get(), actualPayloadLength);
                    }

                    outMsg->sentTime = millis();
                    outMsg->retryCount = 0;
                    outMsg->packetId = packetId;
                    outMsg->state = (final_qos == 1) ? OutgoingQoSState::AwaitingPuback : OutgoingQoSState::AwaitingPubrec;

                    client->outgoingMessages[packetId] = std::move(*outMsg);
                }

                if (actualPayloadLength > 0 && msg->payload)

                {

                    memcpy(ptr, msg->payload.get(), actualPayloadLength);
                }

                client->client->write((const char *)packet.get(), totalPacketLength);

                logMessage(DEBUG_DEBUG, "Retained Message sent: Topic='%s', Payload-length=%u, QoS=%d", msg->topic.c_str(), (unsigned)actualPayloadLength, final_qos);

                break;
            }
        }
    }
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
    String p = password;
    p.trim();

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

    bool passOk = (p == brokerConfig.password);
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

    if (topic.isEmpty())

    {

        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic is empty.");

        return false;
    }

    if (topic.length() > MQTT_MAX_TOPIC_SIZE)

    {

        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' exceeds max length of %d.", topic.c_str(), MQTT_MAX_TOPIC_SIZE);

        return false;
    }

    if (topic.indexOf('#') != -1)

    {

        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains multi-level wildcard '#'.", topic.c_str());

        return false;
    }

    if (topic.indexOf('+') != -1)

    {

        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains single-level wildcard '+'.", topic.c_str());

        return false;
    }

    return true;
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

    if (!topic)

    {

        logMessage(DEBUG_ERROR, "Null pointer as topic for Publish");

        return false;
    }

    if (payloadLen > 0 && !payload)

    {

        logMessage(DEBUG_ERROR, "Null pointer as payload with payloadLen > 0 for Publish");

        return false;
    }

    size_t topicLen = strlen(topic);

    if (topicLen > MQTT_MAX_TOPIC_SIZE)

    {

        logMessage(DEBUG_ERROR, "Topic too long: %u > %u", (unsigned)topicLen, MQTT_MAX_TOPIC_SIZE);

        return false;
    }

    if (payloadLen > MQTT_MAX_PAYLOAD_SIZE)

    {

        logMessage(DEBUG_WARNING, "Payload will be truncated: %u > %u", (unsigned)payloadLen, MQTT_MAX_PAYLOAD_SIZE);

        payloadLen = MQTT_MAX_PAYLOAD_SIZE;
    }

    logMessage(DEBUG_INFO, "📤 Broker is publishing on topic '%s' (Length: %u, QoS: %d, Retained: %s)", topic, (unsigned)payloadLen, qos, retained ? "Yes" : "No");

    if (!excludeClientId.isEmpty())

    {

        logMessage(DEBUG_INFO, "   - Excluded client: %s", excludeClientId.c_str());
    }

    String topicStr = String(topic);

    if (retained)

    {

        retainedMessages.erase(topicStr);

        if (payloadLen > 0)

        {

            auto msg = std::make_unique<RetainedMessage>(topicStr, payload, payloadLen, qos);

            retainedMessages[topicStr] = std::move(msg);
        }
    }

    bool messageSent = false;

    int clientCount = 0;

    int sentCount = 0;

    for (auto &clientEntry : clients)

    {

        auto &c = clientEntry.second;

        if (!c->connected)

            continue;

        clientCount++;

        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            logMessage(DEBUG_DEBUG, "  - Client %s (Original Publisher) will be skipped", c->clientId.c_str());
            continue;
        }

        for (const auto &sub : c->subscriptions)

        {

            if (topicMatches(sub, topicStr))

            {

                // MQTT: Effektiver Zustell-QoS ist der kleinere Wert aus Publish- und Subscription-QoS.
                uint8_t final_qos = (qos < sub.qos) ? qos : sub.qos;

                size_t packet_id_len = (final_qos > 0) ? 2 : 0;

                size_t remainingLength = 2 + topicLen + packet_id_len + payloadLen;

                // Basic check for remaining length encoding

                if (remainingLength > 2097151)

                { // Max for 3 bytes

                    logMessage(DEBUG_ERROR, "Message too large to encode. Topic: %s", topicStr.c_str());

                    continue; // Skip this client
                }

                size_t header_len = 1;

                if (remainingLength <= 127)

                    header_len += 1;

                else if (remainingLength <= 16383)

                    header_len += 2;

                else

                    header_len += 3;

                size_t packetSize = header_len + remainingLength;

                auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[packetSize]);

                uint8_t *ptr = packet.get();

                *ptr++ = (MQTT_PUBLISH << 4) | (final_qos << 1) | (retained ? 1 : 0);

                // Encode remaining length

                size_t rem_len = remainingLength;

                do

                {

                    uint8_t byte = rem_len % 128;

                    rem_len /= 128;

                    if (rem_len > 0)

                    {

                        byte |= 128;
                    }

                    *ptr++ = byte;

                } while (rem_len > 0);

                *ptr++ = topicLen >> 8;

                *ptr++ = topicLen & 0xFF;

                memcpy(ptr, topicStr.c_str(), topicLen);

                ptr += topicLen;

                if (final_qos > 0)

                {

                    uint16_t packetId = getNextPacketId();

                    *ptr++ = packetId >> 8;

                    *ptr++ = packetId & 0xFF;

                    auto outMsg = std::make_unique<OutgoingQoSMessage>();

                    outMsg->qos = final_qos;

                    outMsg->retain = retained;

                    outMsg->topic = topicStr;

                    outMsg->payloadLen = payloadLen;

                    if (payloadLen > 0)

                    {

                        outMsg->payload = std::unique_ptr<uint8_t[]>(new uint8_t[payloadLen]);

                        memcpy(outMsg->payload.get(), payload, payloadLen);
                    }

                    outMsg->sentTime = millis();

                    outMsg->retryCount = 0;

                    outMsg->packetId = packetId;

                    outMsg->state = (final_qos == 1) ? OutgoingQoSState::AwaitingPuback : OutgoingQoSState::AwaitingPubrec;

                    c->outgoingMessages[packetId] = std::move(*outMsg);

                    logMessage(DEBUG_DEBUG, "Storing outgoing QoS %d message for client '%s' (packet ID %u)", final_qos, c->clientId.c_str(), packetId);
                }

                if (payloadLen > 0)

                {

                    memcpy(ptr, payload, payloadLen);
                }

                bool writeSuccess = c->client->write((const char *)packet.get(), packetSize);

                if (writeSuccess)

                {

                    sentCount++;

                    messageSent = true;
                }

                logMessage(DEBUG_DEBUG, "  - Sent PUBLISH to %s (QoS %d), Success: %s", c->clientId.c_str(), final_qos, writeSuccess ? "Yes" : "No");

                break; // Message sent to this client for this topic, move to next client
            }
        }
    }

    logMessage(DEBUG_INFO, "📊 Message sent to %d of %d connected clients", sentCount, clientCount);

    return messageSent;
}

bool ESPAsyncMQTTBroker::isValidTopicFilter(const String &filter)

{

    if (filter.isEmpty())

    {

        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter is empty.");

        return false;
    }

    if (filter.length() > 65535)

    {

        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter exceeds 65535 bytes.");

        return false;
    }

    std::vector<String> levels;

    int start = 0;

    int pos;

    while ((pos = filter.indexOf('/', start)) != -1)

    {

        levels.push_back(filter.substring(start, pos));

        start = pos + 1;
    }

    levels.push_back(filter.substring(start));

    if (levels.empty() && !filter.isEmpty())

    {
    }

    else if (levels.empty() && filter.length() > 0)

    {

        logMessage(DEBUG_WARNING, "Invalid topic filter: Could not split levels for non-empty filter '%s'.", filter.c_str());

        return false;
    }

    for (size_t i = 0; i < levels.size(); ++i)

    {

        const String &level = levels[i];

        if (level.indexOf('#') != -1)

        {

            if (level.length() > 1)

            {

                logMessage(DEBUG_WARNING, "Invalid topic filter: '#' cannot be part of a level (Level: '%s', Filter: '%s').", level.c_str(), filter.c_str());

                return false;
            }

            if (i != levels.size() - 1)

            {

                logMessage(DEBUG_WARNING, "Invalid topic filter: '#' must be the last level (Filter: '%s').", filter.c_str());

                return false;
            }
        }

        else if (level.indexOf('+') != -1)

        {

            if (level.length() > 1)

            {

                logMessage(DEBUG_WARNING, "Invalid topic filter: '+' cannot be part of a level (Level: '%s', Filter: '%s').", level.c_str(), filter.c_str());

                return false;
            }
        }
    }

    return true;
}
