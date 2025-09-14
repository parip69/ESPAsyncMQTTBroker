# Kompatibilitätsreport: ESPAsyncMQTTBroker vs. AsyncMqttClient

## 1. Grundsätzliche Architektur

Dieser Report analysiert die API- und Verhaltenskompatibilität der `ESPAsyncMQTTBroker`-Bibliothek mit der Referenz-Client-Bibliothek `AsyncMqttClient`.

Die fundamentale Erkenntnis ist:
*   **`ESPAsyncMQTTBroker`** ist eine **MQTT-Broker-Implementierung (Server)**. Ihre Aufgabe ist es, Verbindungen von Clients anzunehmen und Nachrichten zwischen ihnen zu vermitteln.
*   **`AsyncMqttClient`** ist eine **MQTT-Client-Implementierung**. Ihre Aufgabe ist es, sich mit einem Broker zu verbinden und Nachrichten zu senden oder zu empfangen.

Eine direkte API-Kompatibilität ist daher nicht gegeben und auch nicht sinnvoll. Die APIs dienen unterschiedlichen Zwecken. Die relevante Kompatibilität liegt auf der **Protokollebene**: Verhält sich der `ESPAsyncMQTTBroker` so, wie es ein Standard-Client wie `AsyncMqttClient` gemäß MQTT 3.1.1 erwartet?

## 2. API-Mapping: Client-Aktion zu Broker-Reaktion

Die folgende Tabelle zeigt, welche Funktion im Broker durch eine typische Aktion eines Clients aufgerufen wird.

| Client-Aktion (`AsyncMqttClient`) | Broker-Handler (`ESPAsyncMQTTBroker`) | Status / Anmerkung |
| :--- | :--- | :--- |
| `connect()` | `onClient()`, `handleConnect()` | **Mapping** |
| `disconnect()` | `handleDisconnect()`, `onDisconnect()` | **Mapping** |
| `publish()` | `handlePublish()` | **Mapping** |
| `subscribe()` | `handleSubscribe()` | **Mapping** |
| `unsubscribe()` | `handleUnsubscribe()` | **Mapping** |
| (Keep-Alive) | `checkTimeouts()`, `handlePingReq()` | **Mapping** |
| `onConnect` Callback | `handleConnect()` sendet `CONNACK` | **Konzeptionell anders** |
| `onDisconnect` Callback | `onDisconnect()` | **Konzeptionell anders** |
| `onMessage` Callback | `publish()` verteilt die Nachricht | **Konzeptionell anders** |

## 3. Semantik-Checkliste (MQTT 3.1.1)

Hier wird das Verhalten des Brokers anhand der MQTT-Spezifikation geprüft.

| Feature | Code-Pfade (`ESPAsyncMQTTBroker.cpp`) | Konformität | Maßnahmen |
| :--- | :--- | :--- | :--- |
| **QoS 0** | `handlePublish()`: Direkte Weiterleitung ohne ACK. `publish()`: Sendet an passende Subscriber. | ✅ **Konform (vermutet)** | Test zur Verifikation nötig. |
| **QoS 1** | `handlePublish()`: Sendet `PUBACK` an den Publisher. (`ESPAsyncMQTTBroker.cpp:602`) | ✅ **Konform (vermutet)** | Test nötig, um DUP-Flag bei Retries zu prüfen (scheint zu fehlen). |
| **QoS 2** | `handlePublish()` -> `incomingQoS2Messages`. `handlePubRec()`, `handlePubRel()`, `handlePubComp()`. | ✅ **Konform (vermutet)** | Kompletter 4-Wege-Handshake muss im Test verifiziert werden. |
| **Retain-Flag** | `publish()`: Speichert Nachricht in `retainedMessages`. `sendRetainedMessages()`: Sendet bei neuem Subscribe. Löschen via leerem Payload in `publish()`. (`.cpp:1150-1157`) | ✅ **Konform (vermutet)** | Test für alle Fälle (Setzen, Senden, Löschen) nötig. |
| **Last Will & Testament (LWT)** | `handleConnect()`: Parsen der Will-Flags. `onDisconnect()`: Publiziert LWT bei unerwartetem Disconnect. (`.cpp:188-194`) | ✅ **Konform (vermutet)** | Test mit simuliertem Verbindungsabbruch erforderlich. |
| **Clean Session** | `handleConnect()`: Flag wird ausgewertet. `onDisconnect()`: `persistentSessions` wird für `cleanSession=false` genutzt. (`.cpp:197-208`, `.cpp:428-436`) | ✅ **Konform (vermutet)** | Test für Persistenz von Subscriptions nötig. |
| **Keep-Alive** | `checkTimeouts()`: Prüft `lastActivity` gegen `keepAlive`. `handlePingReq()`: Antwortet mit `PINGRESP`. (`.cpp:94-113`, `.cpp:911-915`) | ✅ **Konform (vermutet)** | Timeout-Verhalten muss im Test verifiziert werden. |
| **Topic Wildcards (+, #)** | `topicMatches()`: Implementiert die Matching-Logik. `isValidTopicFilter()`: Validiert die Syntax. (`.cpp:945-981`, `.cpp:1230-1282`) | ⚠️ **Unklar/Riskant** | Die `topicMatches`-Implementierung mit `strchr` und `strncmp` ist komplex und anfällig für Edge-Cases (z.B. `a/b` vs `a/b/`, `a/#` vs `a`). Muss intensiv getestet werden. |
| **Packet-ID-Verwaltung** | QoS 1/2 Pakete nutzen Packet-IDs, die vom Client kommen. Der Broker reagiert darauf. | ✅ **Konform** | Der Broker verwaltet keine eigenen IDs, er verarbeitet die des Clients. |
| **DUP-Flag Handling** | DUP-Flag wird im `PUBLISH`-Header vom Client empfangen, aber **nicht** aktiv vom Broker für ausgehende Nachrichten gesetzt. | ❌ **Inkonform** | Der Broker muss bei einem Retry einer QoS > 0 Nachricht das DUP-Flag selbst setzen. Dies scheint zu fehlen. |
| **(UN)SUBACK** | `handleSubscribe()`: Baut `SUBACK` mit passenden Return-Codes. `handleUnsubscribe()`: Sendet `UNSUBACK`. | ✅ **Konform (vermutet)** | Testen. |

## 4. Zusammenfassung und nächste Schritte

Die Broker-Implementierung scheint eine solide Grundlage zu haben und die meisten Konzepte von MQTT 3.1.1 zu berücksichtigen.

**Hauptrisiken und Aufgaben:**
1.  **Fehlendes DUP-Flag-Handling:** Der Broker muss bei erneuter Zustellung von QoS > 0 Nachrichten das DUP-Flag setzen können. Dies ist für volle Konformität erforderlich. **(Fix nötig)**
2.  **Wildcard-Matching-Logik:** Die `topicMatches`-Funktion ist kritisch und muss durch Tests gegen viele Edge-Cases abgesichert werden. **(Test nötig)**
3.  **Keine Client-API:** Um die Kompatibilität zu `AsyncMqttClient` herzustellen, muss ein neuer Kompatibilitäts-Layer oder eine vollwertige Client-Klasse geschrieben werden.

Die nächsten Schritte sind die Implementierung der automatisierten Tests, um die Vermutungen aus dieser Analyse zu bestätigen oder zu widerlegen.
