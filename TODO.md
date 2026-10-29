# TODO – ESPAsyncMQTTBroker

Stand: 29.10.2026. Aktive Arbeitskopie: **2.0.225**.
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
- [x] Tests, Benchmarks und Prüfunterlagen vom Upload ausschließen.
- [x] Dauerhafte Upload-Auswahl in `.gitignore`: nur Bibliotheksquellen, Metadaten, Lizenz und knappe Dokumentation; Test-, Hardware- und Beispieldateien ausschließen.
- [x] Abgeschlossene Testordner lokal aufräumen; vom Benutzer als sauber bestätigt. Rückkehrdateien und kurze Zusammenfassung sind gesichert.
- [ ] Ausgewählte Bibliotheksbeispiele in einer späteren Aufgabe ergänzen.
- [x] Bibliotheksstand 2.0.223 auf GitHub veröffentlichen; nur die elf freigegebenen Bibliotheksdateien.

## Paket 2 abgeschlossen

- [x] Binärpayloads einschließlich eingebetteter Nullbytes durchgängig unverändert weiterleiten; bestehende Text-Callbacks berücksichtigen.
- [x] UTF-8-Prüfung auf die übrigen vorgeschriebenen MQTT-Textfelder erweitern.
- [x] Persistente Sessions einschließlich Löschung und Wiederaufnahme vervollständigen.
- [x] Offline-QoS-Queue für persistente Sessions konzipieren und testen.
- [x] QoS-2-Zustand und Wiederaufnahme vervollständigen; DUP-/PUBREL-Wiederholungen berücksichtigen.
- [x] Ressourcenlimits, erschöpfte Packet-Identifier und Task-Synchronisierung anhand konkreter Fehlerfälle prüfen.

Paket 1 und 2 sind umgesetzt und geprüft. Die RAM-Speicherung und
konfigurierbaren ESP32-Ressourcenlimits sind in README.md dokumentiert.
Die Prüfungen ersetzen keine unabhängige Konformitätszertifizierung.
MQTT 5 und `noLocal` bleiben außerhalb dieses Vorhabens;
`ignoreLoopDeliver` bleibt ohne Laufzeitwirkung.

Normative Grundlage:
[OASIS MQTT 3.1.1 einschließlich Approved Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html).

## Veröffentlichung 2.0.224

- [x] Native Fehlerfallprüfungen und ESP32-Build abschließen.
- [x] Protokoll-, Wiederverbindungs-, Last- und automatische Schaltprüfungen abschließen.
- [x] Temporäre Testordner entfernen; vom Benutzer als sauber bestätigt. Nur notwendige Rückkehrdateien und kurze Zusammenfassung behalten.
- [x] Nur die elf freigegebenen Bibliotheksdateien auf GitHub veröffentlichen.

## Nur bei Bedarf

- [x] Prüfung der physischen 500-ms-Pulsdauer an den Benutzer übergeben; die genaue Messung erfolgt eigenständig.

## Schaltpfadkorrektur vom 22.10.2026

- [x] Fehlenden Retain-Nachweis im bisherigen Anwendungscallback nachvollziehen: Schaltauftrag und gespeicherter Zustand waren dort nicht unterscheidbar.
- [x] Optionale Schnittstelle `onMessageWithRetain` ergänzen, ohne bestehende Text-/Binaercallbacks zu ändern.
- [x] Retain sowohl für QoS 0/1 als auch nach QoS-2-Abschluss weiterreichen; Text nur einmal konvertieren.
- [x] Originalfunktion mit Address-/UndefinedBehaviorSanitizer und beiden Retain-Werten prüfen; Legacy-/Binaercallbacks erhalten: PASS.
- [x] Korrektur direkt im Bibliotheksrepository veröffentlichen. Tests und ausführliches Arbeitsprotokoll lokal behalten, entsprechend der bisherigen Upload-Auswahl.
- [x] Separaten ESP32-/Arduino-Build mit altem Text-/Binaercallback und neuer Retain-Schnittstelle abschliessen: PASS, null Compilerwarnungen.
- [x] Veröffentlichten festen Commit d5a1770ea71d9fb399f3bc320720103525079972 in MAX/MIN/Ro sauber bezogen; alle sechs eingebundenen Bibliotheksdateien byteidentisch. Vollständige Firmware-/LittleFS-Builds 2.4.46 PASS, null Compilerwarnungen; MIN ohne MQTT-Symbole. Hostregressionen 22/22 PASS. Integration mit unveränderter privater OTA-Bibliothek aus Nutzerarchiv geprüft.
- [ ] Reale Wiederanmeldung und Finger-/Web-/MQTT-Impulse mit der neuen Firmware abnehmen. Hosttests ersetzen diese Hardwareprüfung nicht.

## Kleine Stabilitaetskorrektur vom 29.10.2026

Ausgangsstand funktioniert nach Nutzerbestaetigung sehr gut; MQTT-Anmeldung und Schalten sind bereits schnell. Nur belegten Nutzen bei erhaltener Zuverlaessigkeit uebernehmen, ansonsten zuruecknehmen. Keine neuen CPP-/H-Dateien.

- [x] Originale Timerfehler reproduzieren; Erzeugungs-/Startfehler mit Ressourcenfreigabe und Wiederstart absichern. Acht Funktions-/Timer-/Konkurrenzfaelle PASS; normaler Start und Stop bei wartender Wartung erhalten.
- [x] Zwei Loopkandidaten untersuchen und wieder entfernen. Vollstaendige Entfernung der aeusseren Arbeitssperre scheitert am Stop-Konkurrenzfall. Sicherer Fruehreturn senkt isolierte Methodenkosten von 8,117135 auf 0,067175 us, zeigt in wiederholten vollstaendigen Images aber etwa 0,8 ms hoehere MQTT-Mediane und hoehere Main-Looprate. Kein Gesamtvorteil belegt, daher Original-Loop vollstaendig erhalten. Kein pauschaler CPU-/RAM-Gewinn behauptet.
- [x] Verworfenem Fruehreturn 22 MQTT-Faelle, Schaltstatus und 600-s-Lasttest erfolgreich gegenpruefen; diese Funktionsnachweise heben den Laufzeitnachteil nicht auf und ersetzen keine Abnahme des endgueltigen Timerstands.
- [x] Endgueltigen reinen Timerstand ohne Messdiagnose auf .99 pruefen: 22 MQTT-Faelle PASS, Transportmediane wieder im bisherigen Bereich (QoS0 9,901-10,090 ms, QoS1 12,115-12,302 ms), Schaltstatus beider ESPs/Shelly PASS. Normaler geschuetzter OTA-Ablauf mit einem abschliessenden Neustart; Einstellungen erhalten.
- [x] Endgueltiger Timerstand: 600,34 s Last, 2905 Publishes und 8715 gepruefte Zustellungen, 27 Wiederverbindungen, 147 Geraeteabfragen, keine Test-/HTTP-Fehler. Ressourcenstichproben ohne Bootwechsel, neue Fehlerzaehler oder haengende Queues. Kein 24-h-Nachweis daraus ableiten.
- [ ] Erfolgreichen Stand veroeffentlichen, festen Commit regulaer in MAX/MIN/Ro beziehen und Varianten pruefen.
- [ ] Langzeitlauf 24-48 Stunden separat nachweisen; kurzer Lasttest ist kein Ersatz. Menschliche Fingerauflage/elektrische Pulsbreite nicht als automatisch geprueft melden.
