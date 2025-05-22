// @version: 1.5.0 Builddatum 22-05.2025 Parip69
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
{
    auto mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
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
                auto& target = *it; // unique_ptr<MQTTClient>

                // LWT-Veröffentlichung bei unsauberer Trennung
                if (target->hasWill && !target->gracefulDisconnect) {
                    broker->logMessage(DEBUG_INFO, "💥 Unsaubere Trennung von Client %s. Veröffentliche LWT: Topic='%s', QoS=%d, Retain=%s",
                                       target->clientId.c_str(), target->willTopic.c_str(), target->willQos, target->willRetain ? "Ja" : "Nein");
                    
                    // Rufe die (neu erstellte/angepasste) publish-Methode auf, die uint8_t* und Länge akzeptiert.
                    // Die excludeClientId ist hier leer, da der Broker die Nachricht sendet.
                    broker->publish(target->willTopic.c_str(), 
                                    target->willPayload.get(), 
                                    target->willPayloadLen, 
                                    target->willRetain, 
                                    target->willQos, 
                                    ""); // Kein Client ausschließen basierend auf Sender-ID, da Broker sendet

                    target->hasWill = false; // LWT wurde behandelt (versucht zu senden)
                    // Speicher für LWT wird durch unique_ptr und String-Destruktor automatisch freigegeben,
                    // wenn target eventuell später gelöscht wird (cleanSession) oder wenn handleDisconnect bereits LWT geleert hat.
                    // Hier explizit leeren ist nicht mehr nötig, da hasWill = false reicht.
                } else if (target->hasWill && target->gracefulDisconnect) {
                    // Dieser Fall sollte bereits in handleDisconnect behandelt worden sein (hasWill=false gesetzt).
                    // Aber zur Sicherheit hier nochmal loggen, falls doch was durchrutscht.
                    broker->logMessage(DEBUG_DEBUG, "LWT für Client %s nicht gesendet (saubere Trennung bereits in handleDisconnect vermerkt).", target->clientId.c_str());
                }
                
                if (!target->cleanSession) {
                    // Persistenter Client: In die persistente Session-Map verschieben
                    broker->logMessage(DEBUG_INFO, "Client %s getrennt (graceful: %s), Session wird beibehalten.", 
                                     target->clientId.c_str(), target->gracefulDisconnect ? "Ja" : "Nein");
                    
                    broker->persistentSessions[target->clientId] = std::move(target);
                    broker->clients.erase(it);
                } else {
                    // Clean Session: Client entfernen
                    broker->logMessage(DEBUG_INFO, "Client %s getrennt (graceful: %s), Clean Session, entferne Client.",
                                     target->clientId.c_str(), target->gracefulDisconnect ? "Ja" : "Nein");
                    if (broker->clientDisconnectCallback) {
                        broker->clientDisconnectCallback(target->clientId);
                    }
                    broker->connectedClientsInfo.erase(target->clientId);
                    broker->clients.erase(it); // unique_ptr Destruktor gibt MQTTClient frei (inkl. LWT-Daten falls nicht schon in handleDisconnect)
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

    // Prüfung auf leere ClientID in Kombination mit cleanSession=false
    if (clientIdLength == 0 && !cleanSession)
    {
        logMessage(DEBUG_ERROR, "❌ Client-Verbindung abgelehnt: Leere ClientID ist nicht erlaubt für cleanSession=false.");
        uint8_t connack_rejected[] = {0x20, 0x02, 0x00, 0x02}; // Identifier rejected, Session Present = 0
        client->client->write((const char *)connack_rejected, sizeof(connack_rejected));
        client->client->close();
        return;
    }

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
    bool sessionActuallyRestored = false;
    auto sessionIt = persistentSessions.find(clientId);
    if (!cleanSession && sessionIt != persistentSessions.end())
    {
        logMessage(DEBUG_INFO, "♻️ Wiederverwende persistente Session für Client: %s", clientId.c_str());
        client->subscriptions = sessionIt->second->subscriptions;
        persistentSessions.erase(sessionIt);
        sessionActuallyRestored = true;
    }

    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;

    logMessage(DEBUG_DEBUG, "CONNECT Flags: 0x%02X, Clean Session: %s, Keep-Alive: %d Sekunden",
               connectFlags, (cleanSession ? "Ja" : "Nein"), keepAlive);
    logMessage(DEBUG_DEBUG, "Client-ID: '%s'", clientId.c_str());

    // Will-Information verarbeiten, wenn vorhanden
    // Offset zeigt jetzt auf den Beginn der Will-Properties, falls willFlag gesetzt ist.
    if (willFlag)
    {
        client->hasWill = true;
        client->willQos = (connectFlags & 0x18) >> 3;
        client->willRetain = (connectFlags & 0x20) != 0;

        // Parse Will Topic
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Topic-Länge!");
            client->client->close(); // Protokollverstoß, Verbindung schließen
            return;
        }
        uint16_t willTopicLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willTopicLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Topic!");
            client->client->close(); // Protokollverstoß
            return;
        }
        if (willTopicLen > MQTT_MAX_TOPIC_SIZE)
        {
            logMessage(DEBUG_ERROR, "Will-Topic zu lang (%u > %u)! Schließe Verbindung.", willTopicLen, MQTT_MAX_TOPIC_SIZE);
            client->client->close(); // Protokollverstoß
            return;
        }
        // Validiere Will-Topic Namen (darf keine Wildcards enthalten)
        char willTopicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};
        memcpy(willTopicBuffer, data + offset, willTopicLen);
        client->willTopic = String(willTopicBuffer);

        if (!isValidPublishTopic(client->willTopic))
        { // isValidPublishTopic prüft auf Wildcards
            logMessage(DEBUG_ERROR, "❌ Ungültiger Will-Topic Name: '%s' (enthält Wildcards). Schließe Verbindung.", client->willTopic.c_str());
            client->client->close(); // Protokollverstoß
            return;
        }
        offset += willTopicLen;

        // Parse Will Payload
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Payload-Länge!");
            client->client->close(); // Protokollverstoß
            return;
        }
        uint16_t willPayloadActualLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willPayloadActualLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Payload!");
            client->client->close(); // Protokollverstoß
            return;
        }

        client->willPayloadLen = willPayloadActualLen;
        size_t lenToCopy = willPayloadActualLen;

        if (willPayloadActualLen > MQTT_MAX_PAYLOAD_SIZE)
        {
            logMessage(DEBUG_WARNING, "Will-Payload wird auf %u gekürzt (von %u)", MQTT_MAX_PAYLOAD_SIZE, willPayloadActualLen);
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
        offset += willPayloadActualLen; // Offset um die *originale* Länge erhöhen

        logMessage(DEBUG_DEBUG, "LWT registriert: Topic='%s', QoS=%d, Retain=%s, PayloadLen=%u",
                   client->willTopic.c_str(), client->willQos, client->willRetain ? "Ja" : "Nein", client->willPayloadLen);
    }
    else
    {
        client->hasWill = false; // Stellt sicher, dass der Default-Wert gesetzt ist.
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
        (uint8_t)(cleanSession ? 0x00 : (sessionActuallyRestored ? 0x01 : 0x00)),
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

    // Validiere den Topic-Namen
    if (!isValidPublishTopic(topic))
    {
        logMessage(DEBUG_ERROR, "❌ Ungültiger Topic-Name '%s' von Client '%s' empfangen. Schließe Verbindung.",
                   topic.c_str(), client->clientId.c_str());
        if (client->client)
        {                            // Zusätzliche Sicherheitsprüfung
            client->client->close(); // Schließe die Verbindung wegen Protokollverstoß
        }
        return; // Verwirf die Nachricht und führe keine weiteren Aktionen aus
    }

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
        else if (qos == 2)
        {
            // QoS 2: Store message, send PUBREC
            uint32_t payloadLength = length - payloadOffset;
            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
            {
                logMessage(DEBUG_WARNING, "QoS 2 Payload wird auf %u gekürzt (von %u)",
                           MQTT_MAX_PAYLOAD_SIZE, payloadLength);
                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            // Temporäres Speichern der QoS 2 Nachricht
            // Der Payload wird direkt aus `data + payloadOffset` ohne Umweg über payloadBuffer kopiert
            IncomingQoS2Message qos2Msg(topic, data + payloadOffset, payloadLength, retained, client->clientId);
            incomingQoS2Messages[packetId] = std::move(qos2Msg); // Verschiebe in die Map

            logMessage(DEBUG_INFO, "🔔 QoS 2 Publish empfangen – Topic='%s', PacketID=%u. Sende PUBREC.",
                       topic.c_str(), packetId);

            uint8_t pubrec[] = {
                (MQTT_PUBREC << 4), 0x02,
                (uint8_t)(packetId >> 8),
                (uint8_t)packetId};
            client->client->write((const char *)pubrec, 4);
            return; // Nachricht wird erst nach PUBREL weitergeleitet
        }
    }

    // Nur für QoS 0 (und QoS 1, der oben bereits behandelt wurde, aber hier nicht mehr hinkommt)
    // QoS 2 wird oben explizit behandelt und kehrt zurück.
    if (qos == 0)
    {
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
            logMessage(DEBUG_INFO, "🔔 Publish (QoS 0) – Topic='%s', Payload='%s'",
                       topic.c_str(), payloadStr.c_str());

            if (messageCallback)
            {
                messageCallback(client->clientId, topic, payloadStr);
            }

            // Verwende die erweiterte publish-Methode mit noLocal-Unterstützung
            // Die Nachricht weiterleiten, aber den absendenden Client ausschließen
            publish(topic.c_str(), payloadStr.c_str(), retained, qos, client->clientId);
        }
        else if (retained)
        { // QoS 0, leere Payload, aber retained
            logMessage(DEBUG_INFO, "🔔 Publish (QoS 0, leere Retained) – Topic='%s'", topic.c_str());
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
        if (isValidTopicFilter(topic))
        {
            Subscription sub;
            sub.filter = topic;
            sub.noLocal = noLocal; // noLocal wird aus dem options-Byte gelesen
            client->subscriptions.push_back(sub);
            returnCodes.push_back(requestedQoS); // QoS für gültigen Filter
            logMessage(DEBUG_INFO, "✅ Subscription für Client '%s' zu Topic-Filter '%s' hinzugefügt (QoS %d, noLocal %s).",
                       client->clientId.c_str(), topic.c_str(), requestedQoS, noLocal ? "Ja" : "Nein");

            if (subscribeCallback)
            {
                subscribeCallback(client->clientId, topic);
            }
        }
        else
        {
            // Ungültiger Topic-Filter
            logMessage(DEBUG_WARNING, "❌ Subscription für Client '%s' zu ungültigem Topic-Filter '%s' abgelehnt.",
                       client->clientId.c_str(), topic.c_str());
            if (client->protocolVersion == MQTT_PROTOCOL_LEVEL_5)
            {
                returnCodes.push_back(0x8F); // Topic Filter invalid (MQTT 5.0)
            }
            else
            {
                returnCodes.push_back(0x80); // Failure (MQTT 3.1.1)
            }
            // Kein Aufruf des subscribeCallback für ungültige Filter
        }
    }

    // SUBACK senden
    // Ein SUBACK wird auch gesendet, wenn einige/alle Filter ungültig waren,
    // um die entsprechenden Fehlercodes zu übermitteln.
    if (returnCodes.empty())
    {
        logMessage(DEBUG_ERROR, "Keine gültigen Subscriptions im SUBSCRIBE-Paket");
        return;
    } // SUBACK-Paket erstellen
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
    logMessage(DEBUG_INFO, "Ordnungsgemäße Trennung von Client %s (DISCONNECT Paket empfangen).", client->clientId.c_str());
    client->connected = false;
    client->gracefulDisconnect = true; // Markiere als saubere Trennung

    // Verwerfe LWT, da es ein sauberes DISCONNECT ist
    if (client->hasWill)
    {
        logMessage(DEBUG_DEBUG, "LWT für Client %s wird verworfen (saubere Trennung).", client->clientId.c_str());
        client->hasWill = false;
        client->willTopic = "";      // Topic String leeren
        client->willPayload.reset(); // unique_ptr zurücksetzen, gibt Speicher frei
        client->willPayloadLen = 0;
        // willQos und willRetain müssen nicht explizit zurückgesetzt werden, da hasWill=false sie irrelevant macht.
    }

    if (client->cleanSession && client->client)
    {
        client->client->close(); // TCP-Verbindung schließen für Clean Session
    }
    // Für non-clean sessions wird die Verbindung offen gelassen (oder vom Client geschlossen),
    // der onDisconnect Handler kümmert sich dann um das Verschieben in persistentSessions.
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

    auto it = incomingQoS2Messages.find(packetId);
    if (it != incomingQoS2Messages.end())
    {
        IncomingQoS2Message &msg = it->second;

        // Konvertiere Payload von uint8_t[] zu char* für die publish Methode.
        // Stelle sicher, dass der Payload NUL-terminiert ist, falls er als C-String interpretiert wird.
        // Die publish Methode erwartet einen const char* der NUL-terminiert ist.
        // Unsere gespeicherte Payload ist das nicht notwendigerweise.
        String payloadStr;
        if (msg.payload_len > 0 && msg.payload)
        {
            // Erstelle einen String, der auch NUL-Bytes im Payload handhaben kann, falls nötig,
            // aber für die meisten MQTT-Textnachrichten ist eine NUL-Terminierung für C-Strings wichtig.
            // Hier erstellen wir einen Puffer, der sicher NUL-terminiert ist.
            char tempPayload[msg.payload_len + 1];
            memcpy(tempPayload, msg.payload.get(), msg.payload_len);
            tempPayload[msg.payload_len] = '\0';
            payloadStr = String(tempPayload);
        }
        else
        {
            payloadStr = ""; // Leerer Payload
        }

        logMessage(DEBUG_INFO, "PUBREL für Paket-ID %u empfangen. Veröffentliche QoS 2 Nachricht: Topic='%s'",
                   packetId, msg.topic.c_str());

        // Veröffentliche die Nachricht an die Abonnenten
        // QoS wird als 2 beibehalten (obwohl publish() intern dies evtl. für Subscriber anpasst)
        // originalClientId wird als excludeClientId für noLocal Logik verwendet
        publish(msg.topic.c_str(), payloadStr.c_str(), msg.retained, MQTT_QOS2, msg.originalClientId);

        // Entferne die Nachricht aus der Map
        incomingQoS2Messages.erase(it);
    }
    else
    {
        logMessage(DEBUG_WARNING, "PUBREL für unbekannte Paket-ID %u empfangen.", packetId);
        // Spezifikation (MQTT-3.6.2-2): "Regardless of whether the Server has stored state for the
        // Packet Identifier, it MUST send a PUBCOMP packet ..."
    }

    // Sende PUBCOMP in jedem Fall (ob Nachricht gefunden oder nicht)
    uint8_t pubcomp[] = {
        (MQTT_PUBCOMP << 4), 0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)pubcomp, sizeof(pubcomp));
    logMessage(DEBUG_DEBUG, "PUBCOMP für Paket-ID %u gesendet.", packetId);
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
    // Iteriere über die std::map. 'entry' ist ein std::pair<const String, std::unique_ptr<RetainedMessage>>&
    for (auto const &entry : retainedMessages)
    {
        // auto const& topic_key = entry.first; // Das Topic aus dem Map-Key (nicht direkt verwendet, msg->topic ist Quelle der Wahrheit)
        auto const &msg = entry.second; // Der std::unique_ptr<RetainedMessage>

        // Stelle sicher, dass msg gültig ist (sollte es immer sein in der Map, da wir nur per std::move einfügen)
        if (!msg)
        {
            logMessage(DEBUG_ERROR, "Fehler: Ungültiger unique_ptr in retainedMessages Map gefunden.");
            continue;
        }

        for (auto &sub : client->subscriptions)
        {
            // Verwende msg->topic (das im RetainedMessage Objekt gespeicherte Topic)
            // für topicMatches und beim Paketbau.
            if (topicMatches(sub, msg->topic))
            {
                size_t topicLength = msg->topic.length();

                // Überprüfe Topic-Größe
                if (topicLength > MQTT_MAX_TOPIC_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Topic zu lang: %u > %u",
                               (unsigned)topicLength, MQTT_MAX_TOPIC_SIZE);
                    continue; // Zur nächsten Subscription
                }

                // Payload-Länge bestimmen und ggf. kürzen für die Berechnung der totalLength
                size_t actualPayloadLength = msg->length;
                if (msg->length > MQTT_MAX_PAYLOAD_SIZE)
                {
                    logMessage(DEBUG_WARNING, "Retained Payload für Topic '%s' wird gekürzt: %u > %u",
                               msg->topic.c_str(), (unsigned)msg->length, MQTT_MAX_PAYLOAD_SIZE);
                    actualPayloadLength = MQTT_MAX_PAYLOAD_SIZE;
                }

                // Gesamtlänge berechnen (Fixed Header + TopicLen + PayloadLen)
                // Die 'Remaining Length' selbst (packet[1]) darf nicht die ersten 2 Bytes des MQTT-Headers enthalten
                size_t remainingLengthField = 2 + topicLength + actualPayloadLength;
                size_t totalPacketLength = 1 + 1 + remainingLengthField; // Type + RemainingLengthByte(s) + Payload

                // Prüfung, ob die "Remaining Length" in ein einzelnes Byte passt (wie aktuell implementiert)
                if (remainingLengthField > 127)
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) zu groß für einfache Kodierung der Remaining Length: %u.",
                               msg->topic.c_str(), (unsigned)remainingLengthField);
                    continue; // Zur nächsten Subscription
                }

                // Prüfung gegen die absolute Paketgröße
                if (totalPacketLength > MQTT_MAX_PACKET_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) überschreitet MQTT_MAX_PACKET_SIZE: %u > %u.",
                               msg->topic.c_str(), (unsigned)totalPacketLength, MQTT_MAX_PACKET_SIZE);
                    continue; // Zur nächsten Subscription
                }

                // Paket dynamisch allozieren
                std::unique_ptr<uint8_t[]> packet(new uint8_t[totalPacketLength]);
                // Verwende msg->qos für das QoS-Level der Retained Message
                packet[0] = (MQTT_PUBLISH << 4) | (msg->qos << 1) | 0x01; // Retain Flag ist 1
                packet[1] = (uint8_t)remainingLengthField;                // Remaining Length (nur 1 Byte hier)
                packet[2] = topicLength >> 8;
                packet[3] = topicLength & 0xFF;
                memcpy(packet.get() + 4, msg->topic.c_str(), topicLength);

                // Verwende msg->payload.get() und die (ggf. gekürzte) actualPayloadLength für den Payload
                if (actualPayloadLength > 0 && msg->payload)
                {
                    memcpy(packet.get() + 4 + topicLength, msg->payload.get(), actualPayloadLength);
                }

                client->client->write((const char *)packet.get(), totalPacketLength);

                logMessage(DEBUG_DEBUG, "Retained Message gesendet: Topic='%s', Payload-Länge=%u, QoS=%d",
                           msg->topic.c_str(), (unsigned)actualPayloadLength, msg->qos);

                break; // Wichtig: Nachricht wurde für diese Subscription gesendet.
                       // Gehe zur nächsten Retained Message (äußere Schleife).
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

bool ESPAsyncMQTTBroker::isValidPublishTopic(const String &topic)
{
    if (topic.isEmpty())
    {
        logMessage(DEBUG_WARNING, "❌ Ungültiger Publish-Topic: Topic ist leer.");
        return false;
    }

    // Überprüfung der Gesamtlänge (obwohl MQTT_MAX_TOPIC_SIZE dies bereits einschränkt)
    // MQTT_MAX_TOPIC_SIZE ist 256. Die 65535-Grenze ist eher für die absolute MQTT-Spezifikation.
    if (topic.length() > MQTT_MAX_TOPIC_SIZE)
    { // Verwende unsere interne, strengere Grenze
        logMessage(DEBUG_WARNING, "❌ Ungültiger Publish-Topic: Topic '%s' überschreitet maximale Länge von %d.", topic.c_str(), MQTT_MAX_TOPIC_SIZE);
        return false;
    }
    // Die allgemeine MQTT-Grenze von 65535 ist hier weniger relevant als unsere Konfiguration.

    // Prüfe auf Wildcard-Zeichen
    if (topic.indexOf('#') != -1)
    {
        logMessage(DEBUG_WARNING, "❌ Ungültiger Publish-Topic: Topic '%s' enthält Multi-Level Wildcard '#'.", topic.c_str());
        return false;
    }
    if (topic.indexOf('+') != -1)
    {
        logMessage(DEBUG_WARNING, "❌ Ungültiger Publish-Topic: Topic '%s' enthält Single-Level Wildcard '+'.", topic.c_str());
        return false;
    }

    // Optional: Prüfung auf eingebettete NUL-Zeichen, falls Topics als C-Strings behandelt werden (nicht explizit gefordert, aber gute Praxis)
    // if (topic.indexOf((char)0) != -1) {
    //     logMessage(DEBUG_WARNING, "❌ Ungültiger Publish-Topic: Topic '%s' enthält NUL-Zeichen.", topic.c_str());
    //     return false;
    // }

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
    // Parameter-Validierung für C-String Payload
    if (!topic)
    {
        logMessage(DEBUG_ERROR, "Null-Zeiger als Topic für C-String Publish");
        return false;
    }
    if (!payload)
    {
        // Erlaube leeren Payload als leeren String anstatt nullptr
        logMessage(DEBUG_DEBUG, "Null-Zeiger als Payload für C-String Publish, behandle als leeren String.");
        return publish(topic, (const uint8_t *)"", 0, retained, qos, excludeClientId);
    }

    // Rufe die Haupt-Publish-Methode auf, die binären Payload verarbeitet
    // Konvertiere const char* payload zu const uint8_t*
    // strlen wird verwendet, um die Länge des C-Strings zu bestimmen
    return publish(topic, (const uint8_t *)payload, strlen(payload), retained, qos, excludeClientId);
}

bool ESPAsyncMQTTBroker::publish(const char *topic, uint8_t qos, bool retained, const char *payload)
{
    // Ruft die ursprüngliche publish-Methode mit umgekehrter Reihenfolge der Parameter auf
    return publish(topic, payload, retained, qos);
}

// Neue interne Haupt-Publish-Methode, die binären Payload und Länge akzeptiert
bool ESPAsyncMQTTBroker::publish(const char *topic, const uint8_t *payload, size_t payloadLen, bool retained, uint8_t qos, const String &excludeClientId)
{
    // Parameter-Validierung
    if (!topic) // Payload kann nullptr sein, wenn payloadLen 0 ist
    {
        logMessage(DEBUG_ERROR, "Null-Zeiger als Topic für Publish");
        return false;
    }
    if (payloadLen > 0 && !payload)
    {
        logMessage(DEBUG_ERROR, "Null-Zeiger als Payload bei payloadLen > 0 für Publish");
        return false;
    }

    size_t topicLen = strlen(topic);
    if (topicLen > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic zu lang: %u > %u",
                   (unsigned)topicLen, MQTT_MAX_TOPIC_SIZE);
        return false;
    }
    // Die Validierung des Topic-Namens selbst (keine Wildcards) sollte bereits vor dem Aufruf dieser internen Methode erfolgt sein,
    // z.B. in handlePublish oder beim Setzen des Will-Topics. Für Broker-interne Aufrufe (LWT) ist es vertrauenswürdig.

    if (payloadLen > MQTT_MAX_PAYLOAD_SIZE)
    {
        logMessage(DEBUG_WARNING, "Payload wird gekürzt: %u > %u",
                   (unsigned)payloadLen, MQTT_MAX_PAYLOAD_SIZE);
        payloadLen = MQTT_MAX_PAYLOAD_SIZE; // Kürze PayloadLen, wenn es das Maximum überschreitet
    }

    // INFO-Log (Payload wird hier nicht geloggt, da es binär sein kann)
    logMessage(DEBUG_INFO, "📤 Broker veröffentlicht auf Topic '%s' (Länge: %u, QoS: %d, Retained: %s)",
               topic, (unsigned)payloadLen, qos, retained ? "Ja" : "Nein");
    if (!excludeClientId.isEmpty())
    {
        logMessage(DEBUG_INFO, "   - Ausgeschlossener Client: %s", excludeClientId.c_str());
    }

    String topicStr = String(topic);

    // Retained-Nachrichten verwalten
    if (retained)
    {
        retainedMessages.erase(topicStr);

        if (payloadLen > 0)
        {
            auto msg = std::unique_ptr<RetainedMessage>(new RetainedMessage(topicStr,
                                                                            payload, // Direkt den uint8_t* verwenden
                                                                            payloadLen,
                                                                            qos));
            retainedMessages[topicStr] = std::move(msg);
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

        bool isOriginalPublisherAndShouldBeSkipped = false;
        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            bool foundNonNoLocalMatch = false;
            bool hasAnyMatchingSubscription = false;
            for (auto &subCheck : c->subscriptions)
            {
                if (topicMatches(subCheck.filter, topicStr))
                {
                    hasAnyMatchingSubscription = true;
                    if (!subCheck.noLocal)
                    {
                        foundNonNoLocalMatch = true;
                        break;
                    }
                }
            }
            if (hasAnyMatchingSubscription && !foundNonNoLocalMatch)
            {
                isOriginalPublisherAndShouldBeSkipped = true;
                logMessage(DEBUG_DEBUG, "  - Client %s (Original Publisher) wird übersprungen (alle passenden Subscriptions haben noLocal=true)",
                           c->clientId.c_str());
            }
        }

        if (isOriginalPublisherAndShouldBeSkipped)
        {
            continue;
        }

        for (auto &sub : c->subscriptions)
        {
            logMessage(DEBUG_DEBUG, "🔍 Prüfe Topic-Match für Client %s: Abo='%s', Eingang='%s'",
                       c->clientId.c_str(), sub.filter.c_str(), topicStr.c_str());

            bool matched = topicMatches(sub.filter, topicStr);
            logMessage(DEBUG_DEBUG, "  - Match: %s", matched ? "✅ JA" : "❌ NEIN");

            if (matched)
            {
                size_t currentTopicLen = topicStr.length(); // topicLen von oben wiederverwenden
                size_t remainingLength = 2 + currentTopicLen + payloadLen;

                if (remainingLength > 127)
                {
                    logMessage(DEBUG_ERROR, "Nachricht zu groß für einfache Kodierung der Remaining Length: %u. Client: %s, Topic: %s",
                               (unsigned)remainingLength, c->clientId.c_str(), topicStr.c_str());
                    continue; // Zum nächsten Abo oder Client springen, anstatt die ganze Publish-Aktion abzubrechen
                }

                auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[1 + 1 + remainingLength]);
                packet[0] = header;
                packet[1] = remainingLength;
                packet[2] = currentTopicLen >> 8;
                packet[3] = currentTopicLen & 0xFF;
                memcpy(packet.get() + 4, topicStr.c_str(), currentTopicLen);
                if (payloadLen > 0)
                { // Nur kopieren, wenn Payload vorhanden
                    memcpy(packet.get() + 4 + currentTopicLen, payload, payloadLen);
                }

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

                break;
            }
        }

        if (!sentToThisClient && !isOriginalPublisherAndShouldBeSkipped)
        {
            logMessage(DEBUG_DEBUG, "  - Client %s hat keine passenden Subscriptions für Topic %s",
                       c->clientId.c_str(), topicStr.c_str());
        }
    }

    logMessage(DEBUG_INFO, "📊 Nachricht gesendet an %d von %d verbundenen Clients",
               sentCount, clientCount);

    return messageSent;
}

bool ESPAsyncMQTTBroker::isValidTopicFilter(const String &filter)
{
    if (filter.isEmpty())
    {
        logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: Filter ist leer.");
        return false;
    }

    // Überprüfung der Gesamtlänge (obwohl MQTT_MAX_TOPIC_SIZE dies bereits einschränkt)
    if (filter.length() > 65535)
    { // MQTT specific limit
        logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: Filter überschreitet 65535 Bytes.");
        return false;
    }
    // Unsere interne MQTT_MAX_TOPIC_SIZE ist viel restriktiver (256), diese Prüfung ist also eher pro forma.

    // Aufteilen des Filters in Ebenen
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
        // Sollte nicht passieren, wenn filter nicht leer ist, aber als Sicherheitsnetz
        // z.B. wenn filter nur aus "/" besteht, wird levels ["", ""].
        // Wenn filter "a" ist, wird levels ["a"].
    }

    if (filter == "/")
    {   // Spezieller Fall, ergibt ["", ""], was gültig ist.
        // No action needed, valid.
    }
    else if (levels.empty() && filter.length() > 0)
    { // Sollte nicht eintreten, wenn der Splitter korrekt funktioniert
        logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: Konnte Ebenen nicht zerlegen für nicht-leeren Filter '%s'.", filter.c_str());
        return false;
    }

    for (size_t i = 0; i < levels.size(); ++i)
    {
        const String &level = levels[i];

        if (level.indexOf('#') != -1)
        { // Enthält '#'
            if (level.length() > 1)
            { // Mehr als nur '#', z.B. "sport#" oder "#abc"
                logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: '#' darf nicht Teil einer Ebene sein (Ebene: '%s', Filter: '%s').", level.c_str(), filter.c_str());
                return false;
            }
            // Ebene ist genau "#"
            if (i != levels.size() - 1)
            { // '#' ist nicht die letzte Ebene
                logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: '#' muss die letzte Ebene sein (Filter: '%s').", filter.c_str());
                return false;
            }
        }
        else if (level.indexOf('+') != -1)
        { // Enthält '+'
            if (level.length() > 1)
            { // Mehr als nur '+', z.B. "sport+" oder "+abc"
                logMessage(DEBUG_WARNING, "❌ Ungültiger Topic-Filter: '+' darf nicht Teil einer Ebene sein (Ebene: '%s', Filter: '%s').", level.c_str(), filter.c_str());
                return false;
            }
            // Ebene ist genau "+", was gültig ist.
        }
        else
        {
            // Normale Ebene, darf keine Wildcards enthalten (bereits durch obige Checks abgedeckt)
            // und darf nicht leer sein, es sei denn, es ist die erste oder letzte Ebene eines Filters wie "/foo" oder "foo/"
            // Beispiel: "/finance" -> levels ["", "finance"] -> level[0] ist "" -> gültig
            // Beispiel: "finance/" -> levels ["finance", ""] -> level[1] ist "" -> gültig
            // Beispiel: "finance//stock" -> levels ["finance", "", "stock"] -> level[1] ist "" -> gültig
            // Die MQTT-Spezifikation sagt: "Each Topic Level in the Topic Filter and Topic Name MUST NOT be empty" [MQTT-4.7.1-2]
            // Jedoch "foo/" oder "/bar" sind valide Filter. Die Zerlegung in Ebenen mit "" ist hier ein Artefakt.
            // Die Ebenen selbst sind nicht "leer" im Sinne der Spezifikation, sondern der Schrägstrich definiert eine Struktur.
            // Ein Filter wie "a//b" ist gültig und bedeutet, dass die mittlere Ebene leer ist.
            // Der Check `level.isEmpty()` ohne Kontext (erste/letzte Ebene) ist hier nicht zielführend.
            // Die Wildcard-Checks sind das Wichtigste.
        }
    }

    return true;
}
