// ❤️ 📂 🎉❤️ endlich mehere cliens mqtt ❤️ 📂 🎉❤️️
// @ 2.0.216

#include "ESPAsyncMQTTBroker.h"

#include <cstdarg>

// Hilfsfunktion für CONNACK + Close

static inline void sendConnackAndClose(MQTTClient *client, uint8_t returnCode)

{

    uint8_t connack[] = {0x20, 0x02, 0x00, returnCode};

    client->client->write((const char *)connack, sizeof(connack));

    client->client->close();
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

        // Check for client keep-alive timeout
        if (mqttClient->connected && mqttClient->keepAlive > 0 &&
            (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL))
        {
            logMessage(DEBUG_INFO, "Client ⏰ inactive, disconnecting: %s", mqttClient->clientId.c_str());
            AsyncClient *clientToClose = mqttClient->client;
            it++;
            clientToClose->close();
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
                            // Resend PUBLISH with DUP flag
                            size_t topicLen = outMsg.topic.length();
                            size_t packet_id_len = 2;
                            size_t remainingLength = 2 + topicLen + packet_id_len + outMsg.payloadLen;
                            size_t packetSize = 1 + 1 + remainingLength; // simple remaining length for now
                            auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[packetSize]);
                            packet[0] = (MQTT_PUBLISH << 4) | (outMsg.qos << 1) | (outMsg.retain ? 1 : 0) | 0x08; // Set DUP flag
                            packet[1] = remainingLength;
                            packet[2] = topicLen >> 8;
                            packet[3] = topicLen & 0xFF;
                            memcpy(packet.get() + 4, outMsg.topic.c_str(), topicLen);
                            packet[4 + topicLen] = outMsg.packetId >> 8;
                            packet[5 + topicLen] = outMsg.packetId & 0xFF;
                            if (outMsg.payloadLen > 0)
                            {
                                memcpy(packet.get() + 6 + topicLen, outMsg.payload.get(), outMsg.payloadLen);
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



            if (len > MQTT_MAX_PACKET_SIZE) {



                broker->logMessage(DEBUG_ERROR, "Packet size exceeds limit: %u > %u", (unsigned)len, (unsigned)MQTT_MAX_PACKET_SIZE);



                return;



            }



            broker->processPacket(mqttClient, (uint8_t*)data, len);



            mqttClient->lastActivity = millis();



        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *client)

                         {



        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;



        auto it = broker->clients.find(client);



        if (it != broker->clients.end()) {



            auto& target = it->second;







            if (target->hasWill && !target->gracefulDisconnect) {



                broker->logMessage(DEBUG_INFO, "Unclean disconnect from client %s. Publishing LWT: Topic='%s', QoS=%d, Retain=%s",



                                   target->clientId.c_str(), target->willTopic.c_str(), target->willQos, target->willRetain ? "Yes" : "No");



                broker->publish(target->willTopic.c_str(), target->willPayload.get(), target->willPayloadLen, target->willRetain, target->willQos, "");



                target->hasWill = false;



            } else if (target->hasWill && target->gracefulDisconnect) {



                broker->logMessage(DEBUG_DEBUG, "LWT for client %s not sent (clean disconnect already handled).", target->clientId.c_str());



            }







            if (!target->cleanSession) {



                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), session will be kept.",



                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");



                broker->persistentSessions[target->clientId] = std::move(target);



            } else {



                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), Clean Session, removing client.",



                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");



                if (broker->clientDisconnectCallback) {



                    broker->clientDisconnectCallback(target->clientId);



                }



                broker->connectedClientsInfo.erase(target->clientId);



            }



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

void ESPAsyncMQTTBroker::handleConnect(MQTTClient *client, uint8_t *data, uint32_t length)
{
    logMessage(DEBUG_DEBUG, "🔍 MQTT CONNECT Paket empfangen (len=%u)", length);
    if (length < 10)

    {

        logMessage(DEBUG_ERROR, "❌ Paket zu kurz!");

        return;
    }

    // Protokollname

    uint16_t protocolNameLength = (data[0] << 8) | data[1];

    if (protocolNameLength + 2 > length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für Protokollnamen!");

        return;
    }

    char protocolName[MQTT_MAX_TOPIC_SIZE] = {0};

    if (protocolNameLength >= sizeof(protocolName))

    {

        logMessage(DEBUG_ERROR, "❌ Protocol name too long!");

        return;
    }

    memcpy(protocolName, data + 2, protocolNameLength);

    size_t offset = 2 + protocolNameLength;

    if (offset >= length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für Protokoll-Level!");

        return;
    }

    uint8_t protocolLevel = data[offset++];

    client->protocolVersion = protocolLevel;

    // Flags (CONNECT)

    if (offset >= length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für CONNECT-Flags!");

        return;
    }

    uint8_t connectFlags = data[offset++];

    bool cleanSession = (connectFlags & 0x02) != 0;

    bool willFlag = (connectFlags & 0x04) != 0;

    bool passwordFlag = (connectFlags & 0x40) != 0;

    bool usernameFlag = (connectFlags & 0x80) != 0;

    // KeepAlive

    if (offset + 2 > length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für Keep-Alive!");

        return;
    }

    uint16_t keepAlive = (data[offset] << 8) | data[offset + 1];

    offset += 2;

    // ClientID

    if (offset + 2 > length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für Client-ID!");

        return;
    }

    uint16_t clientIdLength = (data[offset] << 8) | data[offset + 1];

    offset += 2;

    if (clientIdLength == 0 && !cleanSession)

    {

        logMessage(DEBUG_ERROR, "REJECT: Empty ClientID not allowed if cleanSession=false");

        sendConnackAndClose(client, 0x02); // Identifier Rejected

        return;
    }

    if (offset + clientIdLength > length)

    {

        logMessage(DEBUG_ERROR, "❌ Zu kurz für komplette Client-ID!");

        return;
    }

    char clientIdBuffer[256] = {0};

    if (clientIdLength >= sizeof(clientIdBuffer))

    {

        logMessage(DEBUG_ERROR, "❌ Client-ID too long!");

        return;
    }

    memcpy(clientIdBuffer, data + offset, clientIdLength);

    String clientId = String(clientIdBuffer);

    client->clientId = clientId;

    offset += clientIdLength;

    // Session-Wiederherstellung

    bool sessionActuallyRestored = false;

    auto sessionIt = persistentSessions.find(clientId);

    if (!cleanSession && sessionIt != persistentSessions.end())

    {

        logMessage(DEBUG_INFO, "♻️ Persistente Session wiederhergestellt für Client: %s", clientId.c_str());

        client->subscriptions = sessionIt->second->subscriptions;

        persistentSessions.erase(sessionIt);

        sessionActuallyRestored = true;
    }

    client->cleanSession = cleanSession;

    client->keepAlive = keepAlive;
    logMessage(DEBUG_INFO, "[BROKER] CONNECT cid=%s kaSec=%d", clientId.c_str(), keepAlive);

    // Will-Handling (falls gesetzt)

    if (willFlag)

    {

        client->hasWill = true;

        client->willQos = (connectFlags & 0x18) >> 3;

        client->willRetain = (connectFlags & 0x20) != 0;

        if (offset + 2 > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Will-Topic-Länge!");

            client->client->close();

            return;
        }

        uint16_t willTopicLen = (data[offset] << 8) | data[offset + 1];

        offset += 2;

        if (offset + willTopicLen > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Will-Topic!");

            client->client->close();

            return;
        }

        if (willTopicLen > MQTT_MAX_TOPIC_SIZE)

        {

            logMessage(DEBUG_ERROR, "Will-Topic too long!");

            client->client->close();

            return;
        }

        char willTopicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};

        memcpy(willTopicBuffer, data + offset, willTopicLen);

        client->willTopic = String(willTopicBuffer);

        if (!isValidPublishTopic(client->willTopic))

        {

            logMessage(DEBUG_ERROR, "Invalid Will-Topic (wildcards) -> close");

            client->client->close();

            return;
        }

        offset += willTopicLen;

        if (offset + 2 > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Will-Payload-Länge!");

            client->client->close();

            return;
        }

        uint16_t willPayloadActualLen = (data[offset] << 8) | data[offset + 1];

        offset += 2;

        if (offset + willPayloadActualLen > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Will-Payload!");

            client->client->close();

            return;
        }

        client->willPayloadLen = willPayloadActualLen;

        size_t lenToCopy = willPayloadActualLen;

        if (lenToCopy > MQTT_MAX_PAYLOAD_SIZE)

        {

            logMessage(DEBUG_WARNING, "Will-Payload wird gekürzt auf %u (von %u)", MQTT_MAX_PAYLOAD_SIZE, willPayloadActualLen);

            lenToCopy = MQTT_MAX_PAYLOAD_SIZE;

            client->willPayloadLen = MQTT_MAX_PAYLOAD_SIZE;
        }

        if (lenToCopy > 0)

        {

            client->willPayload = std::unique_ptr<uint8_t[]>(new uint8_t[lenToCopy]);

            memcpy(client->willPayload.get(), data + offset, lenToCopy);
        }

        else

        {

            client->willPayload = nullptr;
        }

        offset += willPayloadActualLen;
    }

    else

    {

        client->hasWill = false;
    }

    // --- Username/Password Flags & KONFIG-MODUS (für glasklare Diagnose) ---

    const bool cfgUserSet = !brokerConfig.username.isEmpty();

    const bool cfgPassSet = !brokerConfig.password.isEmpty();

    String username;

    String password;

    logMessage(DEBUG_DEBUG,

               "CONNECT: proto='%s'(lvl=%u), flags=0x%02X [clean=%d, will=%d, usr=%d, pwd=%d], keepAlive=%u, clientId='%s'",

               protocolName, (unsigned)protocolLevel, (unsigned)connectFlags,

               (int)cleanSession, (int)willFlag, (int)usernameFlag, (int)passwordFlag,

               (unsigned)keepAlive, clientId.c_str());

    // --- Einheitliches AUTH-Log (keine Klartext-Passwörter) ---

    if (!cfgUserSet)

    {

        // ANON: keine Flags erforderlich
    }

    else if (cfgUserSet && !cfgPassSet)

    {

        // USER_ONLY: Username-Flag MUSS gesetzt sein

        if (!usernameFlag)

        {

            logMessage(DEBUG_ERROR, "REJECT: Mode=USER_ONLY -> username flag missing");

            sendConnackAndClose(client, 0x04); // Bad user name or password

            return;
        }

        // Passwort-Flag darf fehlen/gesetzt sein (Inhalt wird ignoriert)
    }

    else

    {

        // USER_PASS: beide Flags MÜSSEN gesetzt sein

        if (!usernameFlag || !passwordFlag)

        {

            logMessage(DEBUG_ERROR, "REJECT: Mode=USER_PASS -> required flag(s) missing (usr=%d, pwd=%d)",

                       (int)usernameFlag, (int)passwordFlag);

            sendConnackAndClose(client, 0x04); // Bad user name or password

            return;
        }
    }

    // --- Username / Password Strings sicher einlesen ---

    username = "";

    password = "";

    if (usernameFlag)

    {

        if (offset + 2 > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Username-Länge!");

            return;
        }

        uint16_t usernameLen = (data[offset] << 8) | data[offset + 1];

        offset += 2;

        if (offset + usernameLen > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Username!");

            return;
        }

        if (usernameLen >= 256)

        {

            logMessage(DEBUG_ERROR, "Username too long!");

            return;
        }

        char usernameBuffer[256] = {0};

        memcpy(usernameBuffer, data + offset, usernameLen);

        username = String(usernameBuffer);

        offset += usernameLen;
    }

    if (passwordFlag)

    {

        if (offset + 2 > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Password-Länge!");

            return;
        }

        uint16_t passwordLen = (data[offset] << 8) | data[offset + 1];

        offset += 2;

        if (offset + passwordLen > length)

        {

            logMessage(DEBUG_ERROR, "❌ Zu kurz für Password!");

            return;
        }

        if (passwordLen >= 256)

        {

            logMessage(DEBUG_ERROR, "Password too long!");

            return;
        }

        char passwordBuffer[256] = {0};

        memcpy(passwordBuffer, data + offset, passwordLen);

        password = String(passwordBuffer);

        offset += passwordLen;
    }

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
        // --- Zusammenfassung (ohne Passwörter im Klartext) ---

        logMessage(DEBUG_INFO, "--- MQTT Client Connect Info ---");

        logMessage(DEBUG_INFO, "ClientID      : %s", client->clientId.c_str());

        logMessage(DEBUG_INFO, "Username      : '%s' (len=%u)", username.c_str(), (unsigned)username.length());

        logMessage(DEBUG_INFO, "Password      : %s (len=%u)", password.isEmpty() ? "<empty>" : "<set>", (unsigned)password.length());

        logMessage(DEBUG_INFO, "Flags(usr/pwd): %d / %d", (int)usernameFlag, (int)passwordFlag);

        logMessage(DEBUG_INFO, "CleanSession  : %s", cleanSession ? "true" : "false");

        logMessage(DEBUG_INFO, "KeepAlive     : %u", (unsigned)keepAlive);

        logMessage(DEBUG_INFO, "ProtoVersion  : %u", (unsigned)client->protocolVersion);

        logMessage(DEBUG_INFO, "--------------------------------");
    }

    // --- Authentifizierung ---

    logMessage(DEBUG_DEBUG, "Checking authentication…");

    if (!authenticateClient(username, password))

    {

        logMessage(DEBUG_ERROR, "🚫 Authentication failed – Reject (0x04)");

        sendConnackAndClose(client, 0x04); // Bad user name or password

        return;
    }

    logMessage(DEBUG_INFO, "✅ Auth OK – Verbindung akzeptiert");

    // Erfolg: CONNACK senden

    uint8_t connack[] = {0x20, 0x02, (uint8_t)(cleanSession ? 0x00 : (sessionActuallyRestored ? 0x01 : 0x00)), 0x00};

    client->client->write((const char *)connack, sizeof(connack));

    client->connected = true;

    // Callback & Liste führen

    if (clientConnectCallback)

    {

        IPAddress ip = client->client->remoteIP();

        String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);

        clientConnectCallback(client->clientId, ipStr, username, password.length());

        connectedClientsInfo[client->clientId] = ipStr;
    }

    // Retained Messages pushen

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, uint32_t length, uint8_t header)

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

            client->client->close();
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

            incomingQoS2Messages[packetId] = std::move(qos2Msg);

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

        if (payloadLength > 0)

        {

            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)

            {

                logMessage(DEBUG_WARNING, "Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);

                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            // Payload direkt als String ohne 769-Byte Stack-Buffer konstruieren (BP1-03)
            String originalPayload;
            originalPayload.concat((const char *)(data + payloadOffset), payloadLength);

            // NEU: Payload mit Quelle-Präfix versehen
            String newPayload = "source:[" + client->clientId + "];" + originalPayload;
            if (newPayload.length() > MQTT_MAX_PAYLOAD_SIZE)
            {
                logMessage(DEBUG_WARNING, "Payload mit Source-Präfix überschreitet die maximale Größe und wird gekürzt.");
                newPayload = newPayload.substring(0, MQTT_MAX_PAYLOAD_SIZE);
            }

            logMessage(DEBUG_INFO, "🔔 Weiterleiten (QoS %d, von %s) - Topic='%s', NeuerPayload='%s'", qos, client->clientId.c_str(), topic.c_str(), newPayload.c_str());

            if (messageCallback)

            {

                messageCallback(client->clientId, topic, newPayload);
            }

            publish(topic.c_str(), newPayload.c_str(), retained, qos, client->clientId);
        }

        else if (retained)

        {

            logMessage(DEBUG_INFO, "Publish (QoS %d, empty Retained) - Topic='%s'", qos, topic.c_str());

            if (messageCallback)

            {

                messageCallback(client->clientId, topic, "");
            }

            publish(topic.c_str(), "", retained, qos, client->clientId);
        }
    }
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, uint32_t length)

{

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

        bool noLocal = (options & 0x04) != 0;

        logMessage(DEBUG_DEBUG, "Subscribe: Topic '%s', QoS %d, noLocal: %s", topicBuffer, requestedQoS, noLocal ? "true" : "false");

        if (isValidTopicFilter(topic))

        {

            Subscription sub;

            sub.filter = topic;

            sub.noLocal = noLocal;

            client->subscriptions.push_back(sub);

            returnCodes.push_back(requestedQoS);

            logMessage(DEBUG_INFO, "Subscription for client '%s' to topic filter '%s' added (QoS %d, noLocal %s).", client->clientId.c_str(), topic.c_str(), requestedQoS, noLocal ? "Yes" : "No");

            if (subscribeCallback)

            {

                subscribeCallback(client->clientId, topic);
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

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, uint32_t length)

{

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

                    unsubscribeCallback(client->clientId, topic);
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

        client->client->close();
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

    auto it = incomingQoS2Messages.find(packetId);

    if (it != incomingQoS2Messages.end())

    {

        IncomingQoS2Message &msg = it->second;

        String payloadStr;

        if (msg.payload_len > 0 && msg.payload)

        {

            char tempPayload[msg.payload_len + 1];

            memcpy(tempPayload, msg.payload.get(), msg.payload_len);

            tempPayload[msg.payload_len] = '\0';

            payloadStr = String(tempPayload);
        }

        else

        {

            payloadStr = "";
        }

        logMessage(DEBUG_INFO, "PUBREL for packet ID %u received. Publishing QoS 2 message: Topic='%s'", packetId, msg.topic.c_str());

        publish(msg.topic.c_str(), payloadStr.c_str(), msg.retained, MQTT_QOS2, msg.originalClientId);

        incomingQoS2Messages.erase(it);
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

                size_t remainingLengthField = 2 + topicLength + actualPayloadLength;

                size_t totalPacketLength = 1 + 1 + remainingLengthField;

                if (remainingLengthField > 127)

                {

                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) too large for simple Remaining Length encoding: %u.", msg->topic.c_str(), (unsigned)remainingLengthField);

                    continue;
                }

                if (totalPacketLength > MQTT_MAX_PACKET_SIZE)

                {

                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) exceeds MQTT_MAX_PACKET_SIZE: %u > %u.", msg->topic.c_str(), (unsigned)totalPacketLength, MQTT_MAX_PACKET_SIZE);

                    continue;
                }

                std::unique_ptr<uint8_t[]> packet(new uint8_t[totalPacketLength]);

                packet[0] = (MQTT_PUBLISH << 4) | (msg->qos << 1) | 0x01;

                packet[1] = (uint8_t)remainingLengthField;

                packet[2] = topicLength >> 8;

                packet[3] = topicLength & 0xFF;

                memcpy(packet.get() + 4, msg->topic.c_str(), topicLength);

                if (actualPayloadLength > 0 && msg->payload)

                {

                    memcpy(packet.get() + 4 + topicLength, msg->payload.get(), actualPayloadLength);
                }

                client->client->write((const char *)packet.get(), totalPacketLength);

                logMessage(DEBUG_DEBUG, "Retained Message sent: Topic='%s', Payload-length=%u, QoS=%d", msg->topic.c_str(), (unsigned)actualPayloadLength, msg->qos);

                break;
            }
        }
    }
}

bool ESPAsyncMQTTBroker::isUserAllowed(const String &username, const String &userList)
{
    if (username.isEmpty())
        return false;
    String userLower = username;
    userLower.toLowerCase();
    userLower.trim();

    int currentIndex = 0;
    int nextIndex = 0;
    while ((nextIndex = userList.indexOf(',', currentIndex)) != -1)
    {
        String allowedUser = userList.substring(currentIndex, nextIndex);
        allowedUser.toLowerCase();
        allowedUser.trim();
        if (allowedUser == userLower)
        {
            return true;
        }
        currentIndex = nextIndex + 1;
    }

    String lastUser = userList.substring(currentIndex);
    lastUser.toLowerCase();
    lastUser.trim();
    if (lastUser == userLower)
    {
        return true;
    }

    return false;
}

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

                uint8_t final_qos = qos; // We could downgrade QoS here based on subscription, but for now use original.

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
