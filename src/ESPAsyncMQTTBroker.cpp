// @version: 1.6.0 Builddatum 23-07-2024
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

    // Die Smart Pointer kümmern sich automatisch um die Freigabe der MQTTClient-Objekte
    _clients.clear(); // Map leeren
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
    // Iteriere über die Map _clients
    for (auto it = _clients.begin(); it != _clients.end(); /* no increment here */)
    {
        // it->first is AsyncClient*, it->second is std::unique_ptr<MQTTClient>
        MQTTClient* mqttClient = it->second.get(); 

        if (mqttClient->connected && mqttClient->keepAlive > 0)
        {
            // Timeout: 1,5 × KeepAlive (in Millisekunden)
            if (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL)
            {
                logMessage(DEBUG_INFO, "⏰ Client %s (IP: %s) inaktiv, trenne Verbindung.",
                           mqttClient->clientId.c_str(), mqttClient->client->remoteIP().toString().c_str());
                mqttClient->client->close(); 
                // Der onDisconnect-Handler wird aufgerufen, der den Client aus _clients entfernt.
                // Um Probleme mit dem Iterator zu vermeiden, wenn der Client hier direkt entfernt wird
                // (was durch client->close() ausgelöst wird), ist es besser, den Iterator vor dem close() zu sichern
                // oder davon auszugehen, dass onDisconnect die Entfernung korrekt handhabt und der Iterator ungültig wird.
                // Da onDisconnect den Client entfernt, müssen wir den Iterator hier nicht inkrementieren,
                // sondern den nächsten gültigen Iterator bekommen oder einfach die Schleife verlassen, wenn der Client entfernt wurde.
                // Sicherer ist es, den Iterator vor dem close zu inkrementieren, aber das macht die Logik komplizierter,
                // wenn onDisconnect den Eintrag löscht.
                // Alternative: Die Map-Iteration ist nicht ideal, wenn Elemente während der Iteration entfernt werden.
                // Eine Möglichkeit ist, eine Liste der zu schließenden Clients zu sammeln und sie nach der Iteration zu schließen.
                // Für den Moment belassen wir es bei der Annahme, dass close() den onDisconnect-Handler synchron aufruft
                // und der Iterator danach ungültig sein könnte.
                // Die map::erase gibt den nächsten gültigen Iterator zurück, wenn man ihm den aktuellen Iterator übergibt.
                // Jedoch wird erase im onDisconnect-Handler aufgerufen.
                // Wir machen hier ++it und hoffen, dass onDisconnect seine Arbeit macht.
                // Wenn onDisconnect den aktuellen 'it' löscht, wird der ++it problematisch.
                // Daher: Wenn close() zu einem synchronen onDisconnect führt, das den Client entfernt,
                // sollten wir den Iterator vor dem close() inkrementieren oder eine Kopie der Keys machen und darüber iterieren.
                // Für den Moment: Wir loggen und schließen. Der onDisconnect-Handler sollte den Rest erledigen.
                // Der Iterator wird hier vorsichtig behandelt.
                // Wenn client->close() synchron den onDisconnect-Callback auslöst,
                // wird der Client aus _clients entfernt.
                // Um sicherzustellen, dass der Iterator gültig bleibt, inkrementieren wir ihn vor dem potenziellen Löschen.
                auto currentIt = it++;
                currentIt->second->client->close(); // Dies löst onDisconnect aus, was den Client aus _clients entfernt.
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

    client->onData([](void *arg, AsyncClient *aClient, void *data, size_t len)
                   {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->_clients.find(aClient); // Direkter Zugriff über die Map
        
        if (it != broker->_clients.end()) {
            MQTTClient* mqttClient = it->second.get();
            // Überprüfung der Paketgröße für erhöhte Sicherheit
            if (len > MQTT_MAX_PACKET_SIZE) {
                broker->logMessage(DEBUG_ERROR, "Paketgröße überschreitet Limit: %u > %u für Client %s", 
                                 (unsigned)len, (unsigned)MQTT_MAX_PACKET_SIZE, mqttClient->clientId.c_str());
                // Optional: Verbindung schließen oder Fehler an Client senden
                return;
            }
            
            broker->processPacket(mqttClient, (uint8_t*)data, len);
            mqttClient->lastActivity = millis();
        } else {
            broker->logMessage(DEBUG_WARNING, "Daten von unbekanntem AsyncClient empfangen: %s", aClient->remoteIP().toString().c_str());
        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *aClient)
                         {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->_clients.find(aClient); // Direkter Zugriff über die Map
        
        if (it != broker->_clients.end()) {
            std::unique_ptr<MQTTClient> mqttClient = std::move(it->second); // Ownership übernehmen
            broker->_clients.erase(it); // Aus aktiven Clients entfernen

            broker->logMessage(DEBUG_INFO, "🔌 Client %s (IP: %s) getrennt.", 
                               mqttClient->clientId.c_str(), aClient->remoteIP().toString().c_str());

            if (!mqttClient->cleanSession && !mqttClient->clientId.isEmpty()) {
                // Persistente Session speichern
                broker->logMessage(DEBUG_INFO, "Client %s getrennt, persistente Session wird beibehalten.", 
                                 mqttClient->clientId.c_str());
                // Bevor wir es in persistentSessions einfügen, stellen wir sicher, dass keine alte Session mit der gleichen ID existiert.
                // Obwohl dies durch die Logik in handleConnect (Wiederverwendung oder Löschen alter Sessions)
                // unwahrscheinlich sein sollte, ist es eine gute Sicherheitsmaßnahme.
                broker->persistentSessions.erase(mqttClient->clientId); // Alte Session ggf. löschen
                broker->persistentSessions.emplace(mqttClient->clientId, std::move(mqttClient));
            } else {
                // Clean Session oder keine ClientId -> keine Persistenz
                if (broker->clientDisconnectCallback && !mqttClient->clientId.isEmpty()) {
                    broker->clientDisconnectCallback(mqttClient->clientId);
                }
                // Client aus der Info-Map entfernen
                if (!mqttClient->clientId.isEmpty()) {
                    broker->connectedClientsInfo.erase(mqttClient->clientId);
                }
                // Das mqttClient unique_ptr geht hier out of scope und gibt den Speicher frei
            }
        } else {
             broker->logMessage(DEBUG_WARNING, "Unbekannter AsyncClient hat Verbindung getrennt: %s", aClient->remoteIP().toString().c_str());
        } }, this);

    client->onError([](void *arg, AsyncClient *aClient, int8_t error)
                    {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->_clients.find(aClient); // Direkter Zugriff über die Map
        
        if (it != broker->_clients.end()) {
            MQTTClient* mqttClient = it->second.get();
            if (broker->errorCallback) {
                broker->logMessage(DEBUG_ERROR, "Client %s (IP: %s) Fehler: %s (%d)", 
                                 mqttClient->clientId.c_str(), aClient->remoteIP().toString().c_str(), 
                                 aClient->errorToString(error), error);
                broker->errorCallback(mqttClient->clientId, error, aClient->errorToString(error));
            }
        } else {
            broker->logMessage(DEBUG_ERROR, "Fehler von unbekanntem AsyncClient: %s, Fehler: %s (%d)", 
                               aClient->remoteIP().toString().c_str(), aClient->errorToString(error), error);
        } }, this);

    // Smart Pointer in die Map übertragen (Ownership wird übertragen)
    _clients.emplace(client, std::move(mqttClient));

    logMessage(DEBUG_DEBUG, "Neue MQTT-Verbindung akzeptiert (IP: %s). Aktive Clients: %u",
               client->remoteIP().toString().c_str(), (unsigned)_clients.size());
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
    char protocolName[MQTT_MAX_TOPIC_SIZE];
    if (protocolNameLength < sizeof(protocolName)) {
        memcpy(protocolName, data + 2, protocolNameLength);
        protocolName[protocolNameLength] = '\0'; // Nullterminierung
        logMessage(DEBUG_DEBUG, "Protokoll: %s (Client IP: %s)", protocolName, client->client->remoteIP().toString().c_str());
    } else {
        logMessage(DEBUG_ERROR, "❌ Protokollname zu lang (%u >= %u) von IP %s! Verbindung wird abgelehnt.", protocolNameLength, sizeof(protocolName), client->client->remoteIP().toString().c_str());
        // CONNACK mit Fehler senden (z.B. 0x8A - Protocol error) wäre hier angebracht
        // Vereinfacht: einfach return
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
    char clientIdBuffer[256]; // Standardgröße für Client-IDs, könnte auch MQTT_MAX_CLIENT_ID_SIZE sein, falls definiert
    if (clientIdLength == 0) {
        // MQTT v3.1.1 erlaubt leere Client IDs, wenn CleanSession = 1 ist
        // MQTT v5: Server MUST assign a unique ClientID to the Client
        // Hier generieren wir eine, falls erlaubt oder nötig. Fürs Erste: Fehler wenn leer und nicht erwartet.
        // In diesem Broker wird eine leere ClientID derzeit nicht speziell behandelt,
        // außer dass sie zu Problemen führen kann, wenn sie als Key in Maps verwendet wird.
        // Für maximale Sicherheit lehnen wir leere ClientIDs ab, wenn sie nicht explizit unterstützt werden.
        logMessage(DEBUG_ERROR, "❌ Client-ID ist leer (IP: %s). Verbindung wird abgelehnt.", client->client->remoteIP().toString().c_str());
        // CONNACK 0x02 (Identifier rejected)
        uint8_t connack[] = {0x20, 0x02, 0x00, 0x02};
        client->client->write((const char *)connack, 4);
        return;
    }
    if (clientIdLength < sizeof(clientIdBuffer)) {
        memcpy(clientIdBuffer, data + offset, clientIdLength);
        clientIdBuffer[clientIdLength] = '\0'; // Nullterminierung
    } else {
        logMessage(DEBUG_ERROR, "❌ Client-ID zu lang (%u >= %u) von IP %s! Verbindung wird abgelehnt.", clientIdLength, sizeof(clientIdBuffer), client->client->remoteIP().toString().c_str());
        // CONNACK 0x02 (Identifier rejected)
        uint8_t connack[] = {0x20, 0x02, 0x00, 0x02};
        client->client->write((const char *)connack, 4);
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
        char willTopicBuffer[MQTT_MAX_TOPIC_SIZE];
        if (willTopicLen < sizeof(willTopicBuffer)) {
            memcpy(willTopicBuffer, data + offset, willTopicLen);
            willTopicBuffer[willTopicLen] = '\0';
        } else {
            logMessage(DEBUG_ERROR, "❌ Will-Topic zu lang (%u >= %u) von IP %s! Verbindung wird abgelehnt.", willTopicLen, sizeof(willTopicBuffer), client->client->remoteIP().toString().c_str());
            return; // Oder CONNACK mit Fehler
        }
        offset += willTopicLen;

        if (offset + 2 > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Message-Länge!");
            return;
        }
        uint16_t willMsgLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willMsgLen > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Will-Message!");
            return;
        }

        // Will-Message sicher kopieren
        char willMsgBuffer[MQTT_MAX_PAYLOAD_SIZE];
        if (willMsgLen < sizeof(willMsgBuffer)) {
            memcpy(willMsgBuffer, data + offset, willMsgLen);
            willMsgBuffer[willMsgLen] = '\0';
        } else {
            // Nachricht kürzen und loggen
            memcpy(willMsgBuffer, data + offset, sizeof(willMsgBuffer) - 1);
            willMsgBuffer[sizeof(willMsgBuffer) - 1] = '\0';
            logMessage(DEBUG_WARNING, "Will-Message gekürzt auf %u Bytes.", sizeof(willMsgBuffer) - 1);
        }
        offset += willMsgLen; // Offset trotzdem um die ursprüngliche Länge erhöhen

        logMessage(DEBUG_DEBUG, "Will Topic: '%s', Will Message: '%s'", willTopicBuffer, willMsgBuffer);
    }

    // Username & Password verarbeiten, wenn vorhanden
    String username = "";
    String password = "";
    if (usernameFlag) {
        if (offset + 2 > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Username-Länge!");
            return;
        }
        uint16_t usernameLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + usernameLen > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Username!");
            return;
        }

        // Username sicher kopieren
        char usernameBuffer[256]; // Max Länge für Username
        if (usernameLen < sizeof(usernameBuffer)) {
            memcpy(usernameBuffer, data + offset, usernameLen);
            usernameBuffer[usernameLen] = '\0';
            username = String(usernameBuffer);
        } else {
            logMessage(DEBUG_ERROR, "❌ Username zu lang (%u >= %u) von IP %s! Verbindung wird abgelehnt.", usernameLen, sizeof(usernameBuffer), client->client->remoteIP().toString().c_str());
            // CONNACK 0x04 oder 0x05 (Bad username or password / Not authorized)
            uint8_t connack[] = {0x20, 0x02, 0x00, 0x04}; // Bad username or password
            client->client->write((const char *)connack, 4);
            return;
        }
        offset += usernameLen;
    }

    if (passwordFlag) {
        if (offset + 2 > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Password-Länge! (IP: %s)", client->client->remoteIP().toString().c_str());
            return;
        }
        uint16_t passwordLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + passwordLen > length) {
            logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Password! (IP: %s)", client->client->remoteIP().toString().c_str());
            return;
        }

        // Password sicher kopieren
        char passwordBuffer[256]; // Max Länge für Password
        if (passwordLen < sizeof(passwordBuffer)) {
            memcpy(passwordBuffer, data + offset, passwordLen);
            passwordBuffer[passwordLen] = '\0';
            password = String(passwordBuffer);
        } else {
            // Passwort ist zu lang, aber wir kürzen es nicht, sondern lehnen ab.
            logMessage(DEBUG_ERROR, "❌ Password zu lang (%u >= %u) von IP %s! Verbindung wird abgelehnt.", passwordLen, sizeof(passwordBuffer), client->client->remoteIP().toString().c_str());
            uint8_t connack[] = {0x20, 0x02, 0x00, 0x04}; // Bad username or password
            client->client->write((const char *)connack, 4);
            return;
        }
    }

    logMessage(DEBUG_DEBUG, "Authentifizierung wird überprüft für Client IP: %s...", client->client->remoteIP().toString().c_str());

    // Authentifizierung durchführen
    // Die authenticateClient Funktion loggt bereits Details zum Username etc.
    if (!authenticateClient(username, password))
    {
        logMessage(DEBUG_ERROR, "🚫 Authentifizierung fehlgeschlagen für IP %s - Verbindung abgelehnt", client->client->remoteIP().toString().c_str());

        uint8_t connack[] = {0x20, 0x02, 0x00, 0x05}; // Not authorized
        client->client->write((const char *)connack, 4);
        return;
    }
    else
    {
        // Client ID ist jetzt gesetzt, wir können sie im Erfolgsfall loggen
        logMessage(DEBUG_INFO, "✅ Authentifizierung erfolgreich für Client '%s' (IP: %s) - Verbindung akzeptiert", client->clientId.c_str(), client->client->remoteIP().toString().c_str());
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
    char topicBuffer[MQTT_MAX_TOPIC_SIZE];
    memcpy(topicBuffer, data + 2, topicLength); // topicLength wurde bereits gegen MQTT_MAX_TOPIC_SIZE geprüft
    topicBuffer[topicLength] = '\0'; // Nullterminierung
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
        // payloadLength wurde bereits auf MQTT_MAX_PAYLOAD_SIZE gekürzt, falls nötig.
        char payloadBuffer[MQTT_MAX_PAYLOAD_SIZE + 1]; 
        memcpy(payloadBuffer, data + payloadOffset, payloadLength);
        payloadBuffer[payloadLength] = '\0'; // NUL-terminiert für String-Konvertierung

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
    } else if (payloadLength == 0) { // Leerer Payload ist gültig
        logMessage(DEBUG_INFO, "🔔 Publish – Topic='%s', Payload='[leer]'", topic.c_str());
        if (messageCallback) {
            messageCallback(client->clientId, topic, "");
        }
        publish(topic.c_str(), "", retained, qos, client->clientId);
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
        char topicBuffer[MQTT_MAX_TOPIC_SIZE];
        memcpy(topicBuffer, data + index, topicLength); // topicLength wurde bereits gegen MQTT_MAX_TOPIC_SIZE geprüft
        topicBuffer[topicLength] = '\0'; // Nullterminierung
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
        char topicBuffer[MQTT_MAX_TOPIC_SIZE];
        memcpy(topicBuffer, data + index, topicLength); // topicLength wurde bereits gegen MQTT_MAX_TOPIC_SIZE geprüft
        topicBuffer[topicLength] = '\0'; // Nullterminierung
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
                // totalLength wurde bereits gegen MQTT_MAX_PACKET_SIZE geprüft.
                // topicLength und msg->length wurden ebenfalls geprüft.
                
                // Die Berechnung der Remaining Length für das Publish-Paket:
                // 2 Bytes für Topic-Länge + topicLength + msg->length (Payload)
                // Wenn QoS > 0, kommen noch 2 Bytes für Packet ID hinzu.
                // Da Retained Messages hier mit QoS 0 gesendet werden (implizit durch (MQTT_PUBLISH << 4) | 0x01),
                // brauchen wir keine Packet ID.
                size_t remainingLength = 2 + topicLength + msg->length;
                
                // Encoding der Remaining Length (vereinfacht, da <128 angenommen wird, was meistens der Fall ist)
                // Eine vollständige Implementierung würde variable Bytes für Remaining Length berücksichtigen.
                // Hier gehen wir davon aus, dass totalLength (und damit remainingLength) klein genug ist.
                // MQTT_MAX_PACKET_SIZE ist 1024, also kann remainingLength > 127 sein.
                
                // Sicherstellen, dass wir Puffer für die variable Remaining Length haben (max 4 Bytes)
                uint8_t remainingLengthBytes[4];
                size_t remainingLengthEncodedSize = 0;
                size_t tempRemainingLength = remainingLength;
                do {
                    uint8_t byte = tempRemainingLength % 128;
                    tempRemainingLength /= 128;
                    if (tempRemainingLength > 0) {
                        byte |= 128;
                    }
                    remainingLengthBytes[remainingLengthEncodedSize++] = byte;
                } while (tempRemainingLength > 0 && remainingLengthEncodedSize < sizeof(remainingLengthBytes));

                if (tempRemainingLength > 0) { // remainingLength war zu groß für 4 Bytes
                    logMessage(DEBUG_ERROR, "Retained Message: Remaining Length zu groß zum Kodieren (%u).", (unsigned)remainingLength);
                    continue; 
                }

                size_t headerSize = 1 + remainingLengthEncodedSize; // 1 Byte für Fixed Header + Bytes für Remaining Length
                size_t packetSizeForRetained = headerSize + remainingLength;

                if (packetSizeForRetained > MQTT_MAX_PACKET_SIZE) {
                     logMessage(DEBUG_ERROR, "Retained Message (final check) zu groß: %u > %u",
                               (unsigned)packetSizeForRetained, MQTT_MAX_PACKET_SIZE);
                    continue;
                }

                std::unique_ptr<uint8_t[]> packet(new uint8_t[packetSizeForRetained]);
                packet[0] = (MQTT_PUBLISH << 4) | 0x01; // Retained Bit gesetzt, QoS 0
                memcpy(packet.get() + 1, remainingLengthBytes, remainingLengthEncodedSize);
                
                size_t currentOffset = headerSize;
                packet[currentOffset++] = topicLength >> 8;
                packet[currentOffset++] = topicLength & 0xFF;
                memcpy(packet.get() + currentOffset, msg->topic.c_str(), topicLength);
                currentOffset += topicLength;
                
                if (msg->length > 0 && msg->payload) { // Nur kopieren, wenn Payload vorhanden
                    memcpy(packet.get() + currentOffset, msg->payload.get(), msg->length);
                }
                
                client->client->write((const char *)packet.get(), packetSizeForRetained);

                logMessage(DEBUG_DEBUG, "Retained Message gesendet: %s (Topic Länge: %u, Payload Länge: %u, Paket Größe: %u)",
                           msg->topic.c_str(), (unsigned)topicLength, (unsigned)msg->length, (unsigned)packetSizeForRetained);
                
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
    for (auto const& [asyncClient, clientUPtr] : _clients) // Iteriere über die Map
    {
        // clientUPtr is std::unique_ptr<MQTTClient>, use .get() to get raw pointer
        MQTTClient* c = clientUPtr.get();

        if (!c->connected)
            continue;

        clientCount++;
        bool sentToThisClient = false;

        // Sender ausschließen, ganz unabhängig von ignoreLoopDeliver
        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            logMessage(DEBUG_DEBUG, "  - Client %s (Sender) wird übersprungen", c->clientId.c_str());
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
                // noLocal-Flag: falls gesetzt, und der aktuelle Client ist der Sender (obwohl oben schon geprüft),
                // dann nicht senden. Diese Prüfung hier ist spezifisch für das noLocal-Flag der Subscription.
                if (sub.noLocal && !excludeClientId.isEmpty() && c->clientId == excludeClientId)
                {
                    logMessage(DEBUG_DEBUG, "  - Client %s wird übersprungen (noLocal Flag der Subscription)",
                               c->clientId.c_str());
                    break; // Nächste Subscription dieses Clients prüfen (obwohl meist nur eine Subscription pro Topic)
                           // oder besser: nächste Subscription prüfen, da ein Client mehrere Filter haben kann, die matchen
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
