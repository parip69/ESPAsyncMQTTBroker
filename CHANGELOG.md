# Changelog

## v2.0.219 - Oktober 2026
- Leere MQTT-Payloads werden jetzt auch bei `retain=false` korrekt weitergeleitet und an `onMessage` gemeldet
- INFO-Logging zeigt nur Metadaten und Payload-Länge; vollständige Payload nur noch bei `DEBUG_DEBUG`
- `Subscription::noLocal` erhält den sicheren Defaultwert `false`
- Keine Änderung an QoS-Verteilung, `noLocal`-Auswertung, `ignoreLoopDeliver` oder öffentlicher API

## v2.0.218 - Oktober 2026
- Versionsstände in Quellcode und Bibliotheksmetadaten vereinheitlicht
- README an die aktuelle `onMessage(clientId, topic, payload)`-API angepasst
- Dokumentation korrigiert: Broker basiert auf `AsyncTCP`; Payload wird unverändert weitergeleitet
- Keine Änderung am Laufzeitverhalten oder an der öffentlichen API

## v1.5.0 - Update Mai 2025
- Verbesserte Speicherverwaltung mit Smart Pointern
- Unterstützung für MQTT 5.0 Basis-Features
- Erweiterte Callback-Funktionen
- Optimierte Verarbeitung großer Nachrichten
- Verbesserte Dokumentation

## v1.0.0 – Erster stabiler Release
- MQTT-Broker
