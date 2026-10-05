# TODO – ESPAsyncMQTTBroker

Stand: 05.10.2026. Aktive Arbeitskopie: **2.0.223**.
Der bisherige Stand bleibt unter Tag **v2.0.222** erhalten.

## Paket 1 abgeschlossen

- [x] Paketlängen und Packet-Identifier außerhalb CONNECT/SUBSCRIBE prüfen; Remaining-Length-Randfälle korrigieren.
- [x] Mehrbyte-Remaining-Length für große SUBACK-Antworten korrekt kodieren.
- [x] RETAIN-Flag bei Live-Zustellung korrigieren; gespeicherte Retained-Zustellung erhalten.
- [x] Topic-Matcher für leere Ebenen, Slashes, `/#` und die `$`-Wildcard-Sonderregel korrigieren.
- [x] Höchsten passenden Subscription-QoS wählen, begrenzt durch Publish-QoS; Live-Zustellung einmal je Client.
- [x] Retained nur für aktuell angefragte Filter zustellen; erforderliche Zustellungen bei Mehrfilter-SUBSCRIBE erhalten.
- [x] Automatischen Publisher-Ausschluss entfernen; explizites `excludeClientId` bleibt nutzbar.
- [x] Gesamtpaketlimit auf standardmäßig 4096 Byte erweitern und konfigurierbar machen; separate 768-Byte-Payloadgrenze entfernen.
- [x] Allokationen im Matcher, SUBACK und temporären QoS-Zustand verringern.
- [x] Funktionsänderungen in beiden Broker-Hauptdateien übernehmen; Versionsdateien auf 2.0.223 setzen.
- [x] Protokollprüfungen, ESP32-Build, Geräteupload und Last-/Verbindungsprüfungen abschließen.
- [x] Fingererkennung, ESP-Ausgänge und Shelly mit originalem Skript prüfen; Benutzer bestätigt Funktion.
- [x] Erste Shelly-Schaltung nach frischer Anmeldung bei Broker-Neustart prüfen.
- [x] QoS 0/1/2, öffentliche Callbacks, bestehende Topics und Fingerprint-JSON erhalten.
- [x] Normalbetrieb mit QoS 0 und `DEBUG_NONE` herstellen; COM-Ports freigeben.
- [x] Tests, Benchmarks und ausführliche Prüfberichte lokal sichern und vom Upload ausschließen.
- [x] Dauerhafte Upload-Auswahl in `.gitignore`: nur Bibliotheksquellen, Metadaten, Lizenz und knappe Dokumentation; Test-, Hardware- und Beispieldateien ausschließen.
- [ ] Abgeschlossene Testordner lokal aufräumen; Rückkehrdateien und kurze Zusammenfassung sind gesichert. Rekursives Löschen wurde durch die automatische Sicherheitsprüfung blockiert.
- [ ] Ausgewählte Bibliotheksbeispiele in einer späteren Aufgabe ergänzen.
- [ ] GitHub-Veröffentlichung.

## Paket 2: größere MQTT-3.1.1-Arbeiten

- [ ] Binärpayloads einschließlich eingebetteter Nullbytes durchgängig unverändert weiterleiten; bestehende Text-Callbacks berücksichtigen.
- [ ] UTF-8-Prüfung auf die übrigen vorgeschriebenen MQTT-Textfelder erweitern.
- [ ] Persistente Sessions einschließlich Löschung und Wiederaufnahme vervollständigen.
- [ ] Offline-QoS-Queue für persistente Sessions konzipieren und testen.
- [ ] QoS-2-Zustand und Wiederaufnahme vervollständigen; DUP-/PUBREL-Wiederholungen berücksichtigen.
- [ ] Ressourcenlimits, erschöpfte Packet-Identifier und Task-Synchronisierung anhand konkreter Fehlerfälle prüfen.

2.0.223 beansprucht noch keine vollständige MQTT-3.1.1-Konformität.
QoS-Funktionen werden nicht beschnitten. MQTT 5 und `noLocal` bleiben außerhalb
dieses Vorhabens; `ignoreLoopDeliver` bleibt ohne Laufzeitwirkung.

Normative Grundlage:
[OASIS MQTT 3.1.1 einschließlich Approved Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html).

## Nur bei Bedarf

- [ ] Physische 500-ms-Pulsdauer mit geeigneter Messhardware exakt prüfen.
