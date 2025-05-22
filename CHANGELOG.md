# Changelog

Alle nennenswerten Änderungen an diesem Projekt werden in dieser Datei dokumentiert.

Das Format basiert auf [Keep a Changelog](https://keepachangelog.com/de/1.0.0/),
und dieses Projekt hält sich an [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (obwohl wir hier noch keine Versionen deklariert haben).

## [Unreleased] - 2024-07-29

### Added
- **Last Will and Testament (LWT):** Implementierung der LWT-Funktionalität.
  - LWT-Nachrichten (Topic, Payload, QoS, Retain-Flag) werden nun bei der Client-Verbindung (`CONNECT`) korrekt geparst, validiert (Will-Topic darf keine Wildcards enthalten) und im Client-Kontext gespeichert.
  - LWT-Nachrichten werden bei einer unsauberen Client-Trennung (z.B. Timeout, TCP-Verbindungsabbruch ohne vorheriges `DISCONNECT`-Paket) automatisch vom Broker veröffentlicht.
  - Bei einer sauberen Trennung, initiiert durch ein `DISCONNECT`-Paket des Clients, wird die LWT-Nachricht serverseitig verworfen und nicht veröffentlicht.
  - Eine interne `publish`-Methode wurde erweitert bzw. eine neue Überladung geschaffen, um LWT-Payloads, die binäre Daten enthalten können, mit korrekter Längenangabe zu verarbeiten.

### Changed
- **Verbesserte MQTT-Konformität und Robustheit:**
  - **QoS 2 Handling:** Korrekte Implementierung des Nachrichtenflusses für eingehende QoS 2 Nachrichten (Speicherung bei PUBLISH, Weiterleitung nach PUBREL, korrekte PUBREC/PUBCOMP Antworten).
  - **Topic-Filter Validierung:** Ungültige Topic-Filter (z.B. fehlerhafte Wildcard-Verwendung), die von Clients abonniert werden sollen, werden nun serverseitig erkannt und mit entsprechendem Fehlercode im SUBACK-Paket (0x8F für MQTT 5.0, 0x80 für MQTT 3.1.1) abgelehnt.
  - **Topic-Namen Validierung (Publish):** Verhindert das Publizieren auf Topics, die ungültige Zeichen wie Wildcards (`#`, `+`) enthalten. Die Verbindung zum Client wird bei einem solchen Protokollverstoß getrennt.
  - **Client ID Validierung:** Verbindungsversuche mit einer leeren ClientID werden nun korrekt mit Fehlercode 0x02 (Identifier rejected) im CONNACK-Paket abgewiesen, wenn gleichzeitig `cleanSession=false` gesetzt ist.
  - **Session Present Flag:** Das `sessionPresent`-Flag im CONNACK-Paket wird nun präzise gesetzt. Es ist `1`, wenn `cleanSession=false` ist UND eine persistente Session für den Client tatsächlich gefunden und wiederhergestellt wurde, andernfalls `0`.
  - **noLocal MQTT 5.0 Feature:** Korrekte Implementierung und Auswertung des `noLocal`-Flags für Subscriptions. Verhindert zuverlässig den Empfang eigener Nachrichten durch einen Client, wenn alle seine passenden Subscriptions `noLocal=true` gesetzt haben.
- **Performance-Optimierung:**
  - **Retained Messages Speicherung:** Die interne Speicherung von Retained Messages wurde von `std::vector` auf `std::map` (mit dem Topic als Schlüssel) umgestellt. Dies verbessert die Performance beim Zugriff, Einfügen und Löschen von Retained Messages, insbesondere bei einer großen Anzahl gespeicherter Nachrichten.
