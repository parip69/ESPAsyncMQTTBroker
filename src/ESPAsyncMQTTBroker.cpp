// @version: 1.5.0 Builddatum 21-05.2025
#include "ESPAsyncMQTTBroker.h"
#include <cstdarg>

// Zentrale Logging-Funktion
void ESPAsyncMQTTBroker::logMessage(DebugLevel level, const char *format, ...)
{
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
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker()
{
    stop();

    // Die Smart Pointer kümmern sich automatisch um die Freigabe
    clients.clear();
    retainedMessages.clear();
    persistentSessions.clear();
    connectedClientsInfo.clear();
}

void ESPAsyncMQTTBroker::begin()
{
    server.reset(new AsyncServer(port));
    server->onClient([](void *arg, AsyncClient *client)
                     {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        broker->onClient(client); }, this);
    server->begin();

    // Asynchroner Timer: prüft alle 1 Sekunde die Timeouts
    esp_timer_create_args_t timer_args;
    timer_args.callback = [](void *arg)
    {
        ESPAsyncMQTTBroker *broker = (ESPAsyncMQTTBroker *)arg;
        broker->checkTimeouts();
    };
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "mqtt_timeout_timer";
    esp_timer_create(&timer_args, &timeoutTimer);
    esp_timer_start_periodic(timeoutTimer, 1000000); // 1 Sekunde
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
        server.reset(); // Smart Pointer freigeben
    }
}

void ESPAsyncMQTTBroker::checkTimeouts()
{
    uint32_t now = millis();
    for (auto it = clients.begin(); it != clients.end();)
    {
        auto &mqttClient = *it;
        if (mqttClient->connected && mqttClient->keepAlive > 0)
        {
            // Timeout: 1,5 × KeepAlive (in Millisekunden)
            if (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL)
            {
                logMessage(DEBUG_INFO, "⏰ Client %s inaktiv, trenne Verbindung.",
                           mqttClient->clientId.c_str());
                mqttClient->client->close();
                // Der Client wird aus 'clients' beim Disconnect-Handler entfernt
                ++it;
            }
            else
            {
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }
}

void ESPAsyncMQTTBroker::setConfig(const ESPAsyncMQTTBrokerConfig &config)
{
    brokerConfig = config;
    logMessage(DEBUG_INFO, "🔧 MQTT-Broker Konfiguration:");
    logMessage(DEBUG_INFO, "   Username: %s",
               (brokerConfig.username.isEmpty() ? "[leer]" : brokerConfig.username.c_str()));
    logMessage(DEBUG_INFO, "   Passwort: %s",
               (brokerConfig.password.isEmpty() ? "[leer]" : "[gesetzt]"));
    logMessage(DEBUG_INFO, "   Auth erforderlich: %s",
               (brokerConfig.username != "" ? "Ja" : "Nein"));
}

void ESPAsyncMQTTBroker::onClient(AsyncClient *client)
{    auto mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    mqttClient->client = client;
    mqttClient->connected = false;
    mqttClient->lastActivity = millis();
    mqttClient->keepAlive = 0; // Wird im CONNECT gesetzt
    mqttClient->cleanSession = true;

    client->onData([](void *arg, AsyncClient *client, void *data, size_t len)
                   {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        MQTTClient* mqttClient = nullptr;
        
        // Smart Pointer-sichere Iteration
        for (auto& c : broker->clients) {
            if (c->client == client) {
                mqttClient = c.get();
                break;
            }
        }
        
        if (mqttClient) {
            // Überprüfung der Paketgröße für erhöhte Sicherheit
            if (len > MQTT_MAX_PACKET_SIZE) {
                broker->logMessage(DEBUG_ERROR, "Paketgröße überschreitet Limit: %u > %u", 
                                 (unsigned)len, (unsigned)MQTT_MAX_PACKET_SIZE);
                return;
            }
            
            broker->processPacket(mqttClient, (uint8_t*)data, len);
            mqttClient->lastActivity = millis();
        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *client)
                         {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        
        // Mit Smart Pointern umgehen
        for (auto it = broker->clients.begin(); it != broker->clients.end(); ++it) {
            if ((*it)->client == client) {
                auto& target = *it;
                
                if (!target->cleanSession) {
                    // Persistenter Client: In die persistente Session-Map verschieben
                    broker->logMessage(DEBUG_INFO, "Client %s getrennt, Session wird beibehalten", 
                                     target->clientId.c_str());
                    
                    // Persistente Session speichern und aus der Client-Liste entfernen
                    broker->persistentSessions[target->clientId] = std::move(target);
                    broker->clients.erase(it);
                } else {
                    // Callback aufrufen, falls registriert
                    if (broker->clientDisconnectCallback) {
                        broker->clientDisconnectCallback(target->clientId);
                    }
                    
                    // Client aus der Info-Map entfernen
                    broker->connectedClientsInfo.erase(target->clientId);
                    
                    // Aus der Client-Liste entfernen (und automatisch freigeben)
                    broker->clients.erase(it);
                }
                break;
            }
        } }, this);

    client->onError([](void *arg, AsyncClient *client, int8_t error)
                    {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        MQTTClient* mqttClient = nullptr;
        
        // Smart Pointer-sichere Iteration
        for (auto& c : broker->clients) {
            if (c->client == client) {
                mqttClient = c.get();
                break;
            }
        }
        
        if (broker->errorCallback && mqttClient) {
            broker->logMessage(DEBUG_ERROR, "Client %s Fehler: %d", 
                             mqttClient->clientId.c_str(), error);
            broker->errorCallback(mqttClient->clientId, error, "Client Error");
        } }, this);

    // Smart Pointer in den Vector übertragen (Ownership wird übertragen)
    clients.push_back(std::move(mqttClient));

    logMessage(DEBUG_DEBUG, "Neue MQTT-Verbindung akzeptiert (IP: %s)",
               client->remoteIP().toString().c_str());
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "Paket zu kurz für Header (len=%d)", len);
        return;
    }

    uint8_t header = data[0];
    uint8_t packetType = (header >> 4) & 0x0F;

    // Remaining Length decodieren
    size_t multiplier = 1;
    size_t value = 0;
    uint8_t encodedByte;
    size_t idx = 1;
    do
    {
        if (idx >= len)
        {
            logMessage(DEBUG_ERROR, "Paket zu kurz für vollständige Remaining Length");
            return;
        }
        encodedByte = data[idx++];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
        // Protokollschutz gegen zu lange Kodierung
        if (multiplier > 128 * 128 * 128)
        {
            logMessage(DEBUG_ERROR, "Remaining Length hat ungültiges Format");
            return;
        }
    } while ((encodedByte & 128) != 0);

    if (len < idx + value)
    {
        logMessage(DEBUG_ERROR, "Paket unvollständig oder beschädigt");
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
        // QoS 1-Ack vom Client – wir ignorieren es einfach
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
        // Alle anderen Typen, die wir nicht brauchen, ignorieren
        logMessage(DEBUG_DEBUG, "Unbekannter/Unverarbeiteter Pakettyp: %d", packetType);
        break;
    }
}

void ESPAsyncMQTTBroker::handleConnect(MQTTClient *client, uint8_t *data, uint32_t length)
{
    logMessage(DEBUG_DEBUG, "🔍 MQTT CONNECT Paket empfangen:");

    if (length < 10)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz!");
        return;
    }

    // Protokollnamen-Länge sicherstellen
    uint16_t protocolNameLength = (data[0] << 8) | data[1];
    if (protocolNameLength + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Protokollnamen!");
        return;
    }

    // Protokollnamen sicher kopieren (mit NUL-terminierung)
    char protocolName[MQTT_MAX_TOPIC_SIZE] = {0};
    if (protocolNameLength < sizeof(protocolName))
    {
        memcpy(protocolName, data + 2, protocolNameLength);
        logMessage(DEBUG_DEBUG, "Protokoll: %s", protocolName);
    }
    else
    {
        logMessage(DEBUG_ERROR, "Protokollname zu lang!");
        return;
    }

    // Protokoll-Version überprüfen
    size_t offset = 2 + protocolNameLength;
    if (offset >= length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Protokoll-Level!");
        return;
    }

    uint8_t protocolLevel = data[offset];
    client->protocolVersion = protocolLevel;

    logMessage(DEBUG_DEBUG, "Protokoll-Version: %d (%s)",
               protocolLevel,
               protocolLevel == MQTT_PROTOCOL_LEVEL_5 ? "MQTT 5.0" : protocolLevel == MQTT_PROTOCOL_LEVEL ? "MQTT 3.1.1"
                                                                                                          : "Unbekannt");

    offset++; // Protokoll-Level
    if (offset >= length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für CONNECT-Flags!");
        return;
    }

    uint8_t connectFlags = data[offset];
    bool cleanSession = (connectFlags & 0x02) != 0;
    bool willFlag = (connectFlags & 0x04) != 0;
    bool passwordFlag = (connectFlags & 0x40) != 0;
    bool usernameFlag = (connectFlags & 0x80) != 0;

    offset++;
    if (offset + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Keep-Alive!");
        return;
    }
    uint16_t keepAlive = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (offset + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Client-ID!");
        return;
    }
    uint16_t clientIdLength = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (offset + clientIdLength > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für vollständige Client-ID!");
        return;
    }

    // Client-ID sicher kopieren
    char clientIdBuffer[256] = {0};
    if (clientIdLength < sizeof(clientIdBuffer))
    {
        memcpy(clientIdBuffer, data + offset, clientIdLength);
    }
    else
    {
        logMessage(DEBUG_ERROR, "Client-ID zu lang!");
        return;
    }

    String clientId = String(clientIdBuffer);
    client->clientId = clientId;
    offset += clientIdLength;

    // Persistente Session wiederherstellen
    auto sessionIt = persistentSessions.find(clientId);
    if (!cleanSession && sessionIt != persistentSessions.end())
    {
        logMessage(DEBUG_INFO, "♻️ Wiederverwende persistente Session für Client: %s", clientId.c_str());
        client->subscriptions = sessionIt->second->subscriptions;
        persistentSessions.erase(sessionIt);
    }

    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;

    logMessage(DEBUG_DEBUG, "CONNECT Flags: 0x%02X, Clean Session: %s, Keep-Alive: %d Sekunden",
               connectFlags, (cleanSession ? "Ja" : "Nein"), keepAlive);
    logMessage(DEBUG_DEBUG, "Client-ID: '%s'", clientId.c_str());

    // Will-Information verarbeiten, wenn vorhanden
    if (willFlag)
    {
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Topic-Länge!");
            return;
        }
        uint16_t willTopicLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willTopicLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Topic!");
            return;
        }

        // Will-Topic sicher kopieren
        char willTopicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
        if (willTopicLen < sizeof(willTopicBuffer))
        {
            memcpy(willTopicBuffer, data + offset, willTopicLen);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Will-Topic zu lang!");
            return;
        }
        offset += willTopicLen;

        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Message-Länge!");
            return;
        }
        uint16_t willMsgLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willMsgLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Message!");
            return;
        }

        // Will-Message sicher kopieren
        char willMsgBuffer[MQTT_MAX_PAYLOAD_SIZE] = {0};
        if (willMsgLen < sizeof(willMsgBuffer))
        {
            memcpy(willMsgBuffer, data + offset, willMsgLen);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Will-Message zu lang!");
            return;
        }
        offset += willMsgLen;

        logMessage(DEBUG_DEBUG, "Will Topic: '%s', Will Message: '%s'", willTopicBuffer, willMsgBuffer);
    }

    // Username & Password verarbeiten, wenn vorhanden
    String username = "";
    String password = "";
    if (usernameFlag)
    {
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Username-Länge!");
            return;
        }
        uint16_t usernameLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + usernameLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Username!");
            return;
        }

        // Username sicher kopieren
        char usernameBuffer[256] = {0};
        if (usernameLen < sizeof(usernameBuffer))
        {
            memcpy(usernameBuffer, data + offset, usernameLen);
            username = String(usernameBuffer);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Username zu lang!");
            return;
        }
        offset += usernameLen;
    }

    if (passwordFlag)
    {
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Password-Länge!");
            return;
        }
        uint16_t passwordLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + passwordLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Password!");
            return;
        }

        // Password sicher kopieren
        char passwordBuffer[256] = {0};
        if (passwordLen < sizeof(passwordBuffer))
        {
            memcpy(passwordBuffer, data + offset, passwordLen);
            password = String(passwordBuffer);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Password zu lang!");
            return;
        }
    }

    logMessage(DEBUG_DEBUG, "Authentifizierung wird überprüft...");

    // Authentifizierung durchführen
    if (!authenticateClient(username, password))
    {
        logMessage(DEBUG_ERROR, "🚫 Authentifizierung fehlgeschlagen - Verbindung abgelehnt");

        uint8_t connack[] = {0x20, 0x02, 0x00, 0x05};
        client->client->write((const char *)connack, 4);
        return;
    }
    else
    {
        logMessage(DEBUG_INFO, "✅ Authentifizierung erfolgreich - Verbindung akzeptiert");
    }

    uint8_t connack[] = {
        0x20,
        0x02,
        (uint8_t)(cleanSession ? 0x00 : 0x01),
        0x00};
    client->client->write((const char *)connack, 4);
    client->connected = true;

    // Client-Connect-Callback aufrufen
    if (clientConnectCallback)
    {
        IPAddress ip = client->client->remoteIP();
        String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
        clientConnectCallback(client->clientId, ipStr);

        // Speichere Client-Informationen
        connectedClientsInfo[client->clientId] = ipStr;
    }

    // Retained-Nachrichten senden
    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, uint32_t length, uint8_t header)
{
    uint8_t qos = (header & 0x06) >> 1;
    bool retained = (header & 0x01) != 0;

    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Publish-Paket zu kurz");
        return;
    }

    uint16_t topicLength = (data[0] << 8) | data[1];
    if (2 + topicLength > length)
    {
        logMessage(DEBUG_ERROR, "Publish-Paket zu kurz für Topic");
        return;
    }
    if (topicLength > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic zu lang: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);
        return;
    }

    // Topic sicher kopieren
    char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
    memcpy(topicBuffer, data + 2, topicLength);
    String topic = String(topicBuffer);

    size_t payloadOffset = 2 + topicLength;
    uint16_t packetId = 0;
    if (qos > 0)
    {
        if (payloadOffset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "Publish-Paket zu kurz für QoS Packet-ID");
            return;
        }
        packetId = (data[payloadOffset] << 8) | data[payloadOffset + 1];
        payloadOffset += 2;

        if (qos == 1)
        {
            uint8_t puback[] = {
                0x40, 0x02,
                (uint8_t)(packetId >> 8),
                (uint8_t)packetId};
            client->client->write((const char *)puback, 4);
        }
    }

    uint32_t payloadLength = length - payloadOffset;
    if (payloadLength > 0)
    {
        if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
        {
            logMessage(DEBUG_WARNING, "Payload wird auf %u gekürzt (von %u)",
                       MQTT_MAX_PAYLOAD_SIZE, payloadLength);
            payloadLength = MQTT_MAX_PAYLOAD_SIZE;
        }

        // Payload sicher kopieren und terminieren
        char payloadBuffer[MQTT_MAX_PAYLOAD_SIZE + 1] = {0};
        memcpy(payloadBuffer, data + payloadOffset, payloadLength);
        payloadBuffer[payloadLength] = 0; // NUL-terminiert für String-Konvertierung

        String payloadStr = String(payloadBuffer);
        logMessage(DEBUG_INFO, "🔔 Publish – Topic='%s', Payload='%s'",
                   topic.c_str(), payloadStr.c_str());

        if (messageCallback)
        {
            messageCallback(client->clientId, topic, payloadStr);
        }

        // Verwende die erweiterte publish-Methode mit noLocal-Unterstützung
        // Die Nachricht weiterleiten, aber den absendenden Client ausschließen
        publish(topic.c_str(), payloadStr.c_str(), retained, qos, client->clientId);
    }
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Subscribe-Paket zu kurz");
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
            logMessage(DEBUG_ERROR, "Subscribe-Topic zu lang: %u > %u",
                       topicLength, MQTT_MAX_TOPIC_SIZE);
            break;
        }

        // Topic-Filter sicher kopieren
        char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
        memcpy(topicBuffer, data + index, topicLength);
        String topic = String(topicBuffer);
        index += topicLength;

        if (index >= length)
            break;

        // Options-Byte lesen (bei MQTT 5.0 Subscription Options)
        uint8_t options = data[index++];
        uint8_t requestedQoS = options & 0x03;
        bool noLocal = (options & 0x04) != 0; // Bit 2 = noLocal

        logMessage(DEBUG_DEBUG, "Subscribe: Topic '%s', QoS %d, noLocal: %s",
                   topicBuffer, requestedQoS, noLocal ? "true" : "false");

        // Neue Subscription-Struktur verwenden
        Subscription sub;
        sub.filter = topic;
        sub.noLocal = noLocal;
        client->subscriptions.push_back(sub);

        returnCodes.push_back(requestedQoS);

        if (subscribeCallback)
        {
            subscribeCallback(client->clientId, topic);
        }
    }

    // SUBACK senden
    if (returnCodes.empty())
    {
        logMessage(DEBUG_ERROR, "Keine gültigen Subscriptions im SUBSCRIBE-Paket");
        return;
    }    // SUBACK-Paket erstellen
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

    // Retained-Nachrichten für neue Subscriptions senden
    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Unsubscribe-Paket zu kurz");
        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    // UNSUBACK sofort senden
    uint8_t unsuback[4] = {
        0xB0,
        0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)unsuback, 4);

    // Topics verarbeiten
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
            logMessage(DEBUG_ERROR, "Unsubscribe-Topic zu lang: %u > %u",
                       topicLength, MQTT_MAX_TOPIC_SIZE);
            break;
        }

        // Topic sicher kopieren
        char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
        memcpy(topicBuffer, data + index, topicLength);
        String topic = String(topicBuffer);
        index += topicLength;

        // Subscription entfernen
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
    logMessage(DEBUG_DEBUG, "PING von %s beantwortet", client->clientId.c_str());
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)
{
    logMessage(DEBUG_INFO, "Ordnungsgemäße Trennung von Client %s", client->clientId.c_str());
    client->connected = false;
    if (client->cleanSession && client->client)
    {
        client->client->close();
    }
}

void ESPAsyncMQTTBroker::handlePubRec(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "PubRec-Paket zu kurz");
        return;
    }
    uint16_t packetId = (data[0] << 8) | data[1];
    uint8_t pubrel[] = {
        0x62, 0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)pubrel, sizeof(pubrel));
    logMessage(DEBUG_DEBUG, "PUBREC für Paket-ID %u verarbeitet", packetId);
}

void ESPAsyncMQTTBroker::handlePubRel(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "PubRel-Paket zu kurz");
        return;
    }
    uint16_t packetId = (data[0] << 8) | data[1];
    uint8_t pubcomp[] = {
        0x70, 0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)pubcomp, sizeof(pubcomp));
    logMessage(DEBUG_DEBUG, "PUBREL für Paket-ID %u verarbeitet", packetId);
}

void ESPAsyncMQTTBroker::handlePubComp(MQTTClient *client, uint8_t *data, size_t len)
{
    // Für QoS 2 Abschluss keine weitere Aktion erforderlich
    if (len >= 2)
    {
        uint16_t packetId = (data[0] << 8) | data[1];
        logMessage(DEBUG_DEBUG, "PUBCOMP für Paket-ID %u empfangen", packetId);
    }
}

bool ESPAsyncMQTTBroker::topicMatches(const Subscription &subscription, const String &topic)
{
    return topicMatches(subscription.filter, topic);
}

bool ESPAsyncMQTTBroker::topicMatches(const String &subscription, const String &topic)
{
    // Teile die Subscription in Ebenen auf
    std::vector<String> subLevels;
    int start = 0;
    int pos = 0;
    while ((pos = subscription.indexOf('/', start)) != -1)
    {
        subLevels.push_back(subscription.substring(start, pos));
        start = pos + 1;
    }
    subLevels.push_back(subscription.substring(start));

    // Teile das Topic in Ebenen auf
    std::vector<String> topicLevels;
    start = 0;
    while ((pos = topic.indexOf('/', start)) != -1)
    {
        topicLevels.push_back(topic.substring(start, pos));
        start = pos + 1;
    }
    topicLevels.push_back(topic.substring(start));

    // Vergleiche die Ebenen
    int i = 0;
    for (; i < subLevels.size(); i++)
    {
        String subPart = subLevels[i];

        // Der Mehr-Ebenen-Wildcard '#' muss als letztes Element stehen und matcht alle restlichen Ebenen
        if (subPart == "#")
        {
            return true;
        }

        // Die Ein-Ebenen-Wildcard '+' matcht genau eine Ebene
        if (subPart == "+")
        {
            // Es muss eine entsprechende Topic-Ebene vorhanden sein
            if (i >= topicLevels.size())
                return false;
            // Gehe zur nächsten Ebene
            continue;
        }

        // Für alle anderen Fälle muss die Ebene exakt übereinstimmen
        if (i >= topicLevels.size() || subPart != topicLevels[i])
        {
            return false;
        }
    }

    // Nach Vergleich aller Subscription-Ebenen darf es keine zusätzlichen Topic-Ebenen geben
    return (i == topicLevels.size());
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client)
{
    for (auto &msg : retainedMessages)
    {
        for (auto &sub : client->subscriptions)
        {
            if (topicMatches(sub, msg->topic))
            {
                size_t topicLength = msg->topic.length();

                // Überprüfe Größe
                if (topicLength > MQTT_MAX_TOPIC_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Topic zu lang: %u > %u",
                               (unsigned)topicLength, MQTT_MAX_TOPIC_SIZE);
                    continue;
                }

                // Gesamtlänge berechnen und prüfen
                size_t totalLength = 1 + 1 + 2 + topicLength + msg->length;
                if (totalLength > MQTT_MAX_PACKET_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Message zu groß: %u > %u",
                               (unsigned)totalLength, MQTT_MAX_PACKET_SIZE);
                    continue;
                }

                // Paket auf Stack erstellen (falls es passt) oder dynamisch allozieren
                if (totalLength <= MQTT_MAX_PACKET_SIZE)
                {
                    std::unique_ptr<uint8_t[]> packet(new uint8_t[totalLength]);
                    packet[0] = (MQTT_PUBLISH << 4) | 0x01;
                    packet[1] = totalLength - 2;
                    packet[2] = topicLength >> 8;
                    packet[3] = topicLength & 0xFF;
                    memcpy(packet.get() + 4, msg->topic.c_str(), topicLength);
                    memcpy(packet.get() + 4 + topicLength, msg->payload.get(), msg->length);
                    client->client->write((const char *)packet.get(), totalLength);

                    logMessage(DEBUG_DEBUG, "Retained Message gesendet: %s (Länge: %u)",
                               msg->topic.c_str(), (unsigned)msg->length);
                }
                break;
            }
        }
    }
}

bool ESPAsyncMQTTBroker::authenticateClient(const String &username, const String &password)
{
    logMessage(DEBUG_DEBUG, "🔐 Authentifizierungsversuch:");
    logMessage(DEBUG_DEBUG, "   • Empfangener Username: '%s'", username.c_str());
    logMessage(DEBUG_DEBUG, "   • Passwort: %s", (password.isEmpty() ? "[leer]" : "********"));
    logMessage(DEBUG_DEBUG, "   • Konfigurierter Username: '%s'", brokerConfig.username.c_str());
    logMessage(DEBUG_DEBUG, "   • Konfiguriertes Passwort: %s",
               (brokerConfig.password.isEmpty() ? "[leer]" : "********"));

    // 1. Wenn kein Username konfiguriert ist → anonyme Verbindung erlauben
    if (brokerConfig.username.isEmpty())
    {
        logMessage(DEBUG_INFO, "✅ Anonymer Zugriff erlaubt (kein Benutzername konfiguriert)");
        return true;
    }

    // 2. Wenn konfigurierter Username vorhanden, aber Client sendet keinen
    if (username.isEmpty())
    {
        logMessage(DEBUG_ERROR, "❌ Verbindung ohne Benutzername nicht erlaubt");
        return false;
    }

    // 3. Username muss übereinstimmen
    if (username != brokerConfig.username)
    {
        logMessage(DEBUG_ERROR, "❌ Falscher Benutzername");
        return false;
    }

    // 4. Wenn Passwort nicht konfiguriert ist → Username reicht aus
    if (brokerConfig.password.isEmpty())
    {
        logMessage(DEBUG_INFO, "✅ Benutzername korrekt, kein Passwort erforderlich");
        return true;
    }

    // 5. Wenn Passwort konfiguriert ist → muss exakt stimmen
    if (password != brokerConfig.password)
    {
        logMessage(DEBUG_ERROR, "❌ Falsches Passwort");
        return false;
    }

    logMessage(DEBUG_INFO, "✅ Benutzername und Passwort korrekt");
    return true;
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const char *payload, bool retained, uint8_t qos)
{
    // Die Basis-Methode ruft die erweiterte Methode mit leerer excludeClientId auf
    return publish(topic, payload, retained, qos, "");
}

bool ESPAsyncMQTTBroker::publish(const char *topic,
                                 const char *payload,
                                 bool retained,
                                 uint8_t qos,
                                 const String &excludeClientId)
{
    // Parameter-Validierung
    if (!topic || !payload)
    {
        logMessage(DEBUG_ERROR, "Null-Zeiger als Topic oder Payload");
        return false;
    }

    size_t topicLen = strlen(topic);
    if (topicLen > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic zu lang: %u > %u",
                   (unsigned)topicLen, MQTT_MAX_TOPIC_SIZE);
        return false;
    }

    size_t payloadLen = strlen(payload);
    if (payloadLen > MQTT_MAX_PAYLOAD_SIZE)
    {
        logMessage(DEBUG_WARNING, "Payload wird gekürzt: %u > %u",
                   (unsigned)payloadLen, MQTT_MAX_PAYLOAD_SIZE);
        payloadLen = MQTT_MAX_PAYLOAD_SIZE;
    }

    // INFO-Log
    logMessage(DEBUG_INFO, "📤 Broker veröffentlicht auf Topic '%s': %s", topic, payload);
    if (!excludeClientId.isEmpty())
    {
        logMessage(DEBUG_INFO, "   - Ausgeschlossener Client: %s", excludeClientId.c_str());
    }

    String topicStr = String(topic);

    // Retained-Nachrichten verwalten
    if (retained)
    {
        // Entferne vorhandene retained-Nachricht mit gleichem Topic
        for (auto it = retainedMessages.begin(); it != retainedMessages.end();)
        {
            if ((*it)->topic == topicStr)
            {
                it = retainedMessages.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Bei nicht-leerem Payload: Neue retained-Nachricht speichern
        if (payloadLen > 0)
        {            auto msg = std::unique_ptr<RetainedMessage>(new RetainedMessage(topicStr,
                                                           (const uint8_t *)payload,
                                                           payloadLen,
                                                           qos));
            retainedMessages.push_back(std::move(msg));
        }
    }

    // Header mit QoS- und Retain-Bits
    uint8_t header = (MQTT_PUBLISH << 4) | (qos << 1) | (retained ? 1 : 0);

    bool messageSent = false;
    int clientCount = 0;
    int sentCount = 0;

    // An alle relevanten Clients senden
    for (auto &c : clients)
    {
        if (!c->connected)
            continue;

        clientCount++;
        bool sentToThisClient = false;

        // Sender ausschließen, ganz unabhängig von ignoreLoopDeliver
        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            continue;
        }

        // Durch alle Subscriptions des Clients iterieren
        for (auto &sub : c->subscriptions)
        {
            logMessage(DEBUG_DEBUG, "🔍 Prüfe Topic-Match für Client %s: Abo='%s', Eingang='%s'",
                       c->clientId.c_str(), sub.filter.c_str(), topicStr.c_str());

            bool matched = topicMatches(sub.filter, topicStr);
            logMessage(DEBUG_DEBUG, "  - Match: %s", matched ? "✅ JA" : "❌ NEIN");

            if (matched)
            {
                // noLocal-Flag: falls gesetzt, nochmals sicherstellen, dass der Sender nicht bekommt
                if (sub.noLocal && c->clientId == excludeClientId)
                {
                    logMessage(DEBUG_DEBUG, "  - Client %s wird übersprungen (noLocal)",
                               c->clientId.c_str());
                    break;
                }

                // Paket zusammenbauen
                size_t topicLen = topicStr.length();
                size_t remainingLength = 2 + topicLen + payloadLen;

                if (remainingLength > 127)
                {
                    logMessage(DEBUG_ERROR, "Nachricht zu groß für einfache Kodierung: %u",
                               (unsigned)remainingLength);
                    continue;
                }

                // Paket mit Smart Pointer erstellen
                auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[1 + 1 + remainingLength]);
                packet[0] = header;
                packet[1] = remainingLength; // bleibt < 128 Bytes
                packet[2] = topicLen >> 8;
                packet[3] = topicLen & 0xFF;
                memcpy(packet.get() + 4, topicStr.c_str(), topicLen);
                memcpy(packet.get() + 4 + topicLen, payload, payloadLen);

                logMessage(DEBUG_DEBUG, "📦 Sende Paket an Client ID: %s (Länge: %u)",
                           c->clientId.c_str(), (unsigned)(1 + 1 + remainingLength));

                bool writeSuccess = c->client->write((const char *)packet.get(), 1 + 1 + remainingLength);

                if (writeSuccess)
                {
                    sentCount++;
                    sentToThisClient = true;
                    messageSent = true;
                }

                logMessage(DEBUG_DEBUG, "  - Senden %s",
                           writeSuccess ? "erfolgreich" : "fehlgeschlagen");

                break; // pro Client nur einmal senden
            }
        }

        if (!sentToThisClient)
        {
            logMessage(DEBUG_DEBUG, "  - Client %s hat keine passenden Subscriptions für Topic %s",
                       c->clientId.c_str(), topicStr.c_str());
        }
    }

    logMessage(DEBUG_INFO, "📊 Nachricht gesendet an %d von %d verbundenen Clients",
               sentCount, clientCount);

    return messageSent;
}

bool ESPAsyncMQTTBroker::publish(const char *topic, uint8_t qos, bool retained, const char *payload)
{
    // Ruft die ursprüngliche publish-Methode mit umgekehrter Reihenfolge der Parameter auf
    return publish(topic, payload, retained, qos);
}
