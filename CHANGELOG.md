# Changelog

## v2.0.221 - Oktober 2026
- Subscription-QoS wird beim SUBSCRIBE in `Subscription` gespeichert und bei einem erneuten SUBSCRIBE aktualisiert
- Effektiver Zustell-QoS ist jetzt der kleinere Wert aus Publish-QoS und Subscription-QoS
- Dieselbe QoS-Begrenzung wird auch bei Retained Messages verwendet
- Retained-Zustellungen mit QoS 1/2 enthalten jetzt die erforderliche Packet-ID und werden im vorhandenen Outgoing-QoS-State nachverfolgt
- Keine Änderung an Payload, Topics, `excludeClientId`, `noLocal` oder `ignoreLoopDeliver`
- Öffentliche Broker-Funktionen und bestehende Fingerprint-Aufrufe bleiben unverändert
- Versionsstände auf 2.0.221 angehoben

## v2.0.220 - Oktober 2026
- Reine Aufräum- und Dokumentationsversion ohne Änderung des Laufzeitverhaltens
- `ignoreLoopDeliver` im Code und in der Dokumentation klar als aktuell nicht ausgewertet markiert
- `noLocal`-Kommentare an den tatsächlichen Stand angepasst: wird gespeichert, aber bei der Zustellung noch nicht ausgewertet
- Kommentar bei `final_qos = qos` korrigiert: Subscription-QoS wird aktuell noch nicht gespeichert oder berücksichtigt
- README um eine Übersicht zu unterstützten, teilweise unterstützten und noch nicht umgesetzten Funktionen ergänzt
- Versionsstände auf 2.0.220 vereinheitlicht
- Keine Änderung an QoS-Verteilung, `noLocal`-Logik, `ignoreLoopDeliver`, öffentlicher API oder Projektstruktur

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
