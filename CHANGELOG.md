# Changelog

## v2.0.223 - Oktober 2026
- Konfigurierbares Gesamtpaketlimit von standardmäßig 4096 Byte; vollständige Shelly-Statusmeldungen, keine separate 768-Byte-Payloadgrenze und keine stille Kürzung innerhalb des Limits.
- Paketlängen, Packet-Identifier und Remaining Length prüfen; UNSUBSCRIBE vor Änderungen vollständig validieren; große SUBACK-Antworten korrekt kodieren.
- Live-RETAIN, Topic-Matcher einschließlich `$`, höchste passende Subscription-QoS und Retained-Auswahl je angefragtem Filter korrigieren.
- Publisher mit passendem Abonnement erhält seine Nachricht; explizites `excludeClientId` bleibt erhalten.
- Allokationen im Matcher, SUBACK und temporären QoS-Zustand verringern; QoS 0/1/2 und öffentliche Callbacks erhalten.
- Vollständige Sessions, Offline-Queue, QoS-2-Wiederaufnahme und Binärpayload-Weiterleitung bleiben offen; noch keine vollständige MQTT-3.1.1-Konformität.

## v2.0.222 - Oktober 2026
- Empfang nach synchronem oder verzögertem Close sicher abbrechen; Clientzuordnung nach Handlern und relevanten Anwendungs-Callbacks erneut prüfen
- Internes `closing`-/`connectSeen`-Tracking ohne Änderung der öffentlichen Broker-API
- Zentrale Fixed-Header-Prüfung; ungültige Header und unzulässige Client-Eingangsarten schließen die Verbindung (MQTT-2.2.2-1/-2, MQTT-3.3.1-2/-4)
- Genau ein CONNECT pro Verbindung; keine normalen Requests vor erfolgreicher Annahme (MQTT-3.1.0-1/-2, MQTT-3.1.4-5)
- CONNECT prüft Protokollname, Level 4, Flags, vollständige Felder und UTF-8; Session/Will erst nach erfolgreicher Validierung und Authentifizierung übernehmen (MQTT-3.1.2-1/-2/-3/-11/-14/-15/-22, MQTT-3.1.4-1)
- SUBSCRIBE mit zwei Durchläufen validieren; ID 0, leere/abgeschnittene Payload, ungültiges UTF-8, Filter und Optionsbytes vor jeder Subscription-Änderung abweisen (MQTT-2.3.1-1, MQTT-3.8.3-1/-3, MQTT-3-8.3-4)
- Kein `noLocal` aus reservierten MQTT-3.1.1-Bits; internes Feld bleibt `false`
- Subscription-QoS, bestehende Retained-/QoS-Verteilung, `excludeClientId`, `ignoreLoopDeliver`, JSON und Topics funktional erhalten
- MQTT 5 bleibt nicht unterstützt; bekannte weitere MQTT-3.1.1-Abweichungen bewusst offen
- Hardwareprüfung abgeschlossen: 55 echte Broker-Tests PASS, 0 FAIL; geprüfte Finger-/MQTT-/Ausgangspfade mit Parip99 .99, gumi77 .77 und Shelly .127 bestätigt
- Normalzustand wiederhergestellt: Subscription-QoS 0, DEBUG_NONE, unverändertes Shelly-Skript, Test-Retained gelöscht; genaue physische 500-ms-Pulsdauer nicht separat bestätigt
- Stabiler Stand unter Tag v2.0.222 eingefroren; weitere MQTT-3.1.1-Restpunkte ausschließlich in einer folgenden Version bearbeiten

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
