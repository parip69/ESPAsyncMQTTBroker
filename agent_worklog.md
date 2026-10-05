# ESPAsyncMQTTBroker 2.0.222 – Änderungs- und Prüfbericht

Datum: 05.10.2026. Ausgangsbasis: hardwaregetestete Version 2.0.221,
Commit `1b669bf`. Bearbeitet wurde ausschließlich die Brokerbibliothek.
Der Projektinhaber übernimmt die Integration und Hardwareprüfung im
Fingerprint-Projekt. Dieses Projekt und dessen Bibliothekscache wurden nicht
verändert; es erfolgte kein Firmware-Upload.

## Änderungen und normative Grundlage

Normative Quelle: [OASIS MQTT 3.1.1 einschließlich Approved Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html).

| Bereich | Ursache und Änderung | Anforderung |
|---|---|---|
| Client-Lebensdauer | `close()` kann synchron den Disconnect-Callback auslösen und den MQTTClient löschen oder in eine persistente Session verschieben. `closeMQTTClient()` setzt vorher `closing`; die Empfangsschleife prüft danach die aktuelle Map-Zuordnung ohne Dereferenzieren des alten Zeigers. Relevante CONNECT-/Subscription-Callbacks werden ebenfalls abgesichert. Keine neue Besitzarchitektur. | Interne Speichersicherheit bei den von MQTT vorgeschriebenen Verbindungsabbrüchen; keine eigene MQTT-Anforderungsnummer für C++-Lebensdauer. |
| Fixed Header | Flags wurden zuvor nicht zentral geprüft. `processPacket()` prüft alle unterstützten Client-Pakettypen vor dem Handler; reservierte und reine Server-Pakettypen werden abgewiesen. PUBLISH QoS 3 sowie DUP bei QoS 0 werden abgewiesen, gültige PUBREL-Wiederholungen bleiben erlaubt. | MQTT-2.2.2-1/-2; MQTT-3.3.1-2/-4 |
| Verbindungszustand | Handler konnten vor CONNECT aufgerufen werden; ein zweites CONNECT war möglich. `connectSeen` und das bestehende `connected` begrenzen die Verarbeitung. CONNECT mit direkt folgenden Paketen im selben Empfang bleibt zulässig. | MQTT-3.1.0-1/-2; MQTT-3.1.4-5 |
| CONNECT | Der Zustand wurde bereits während des Parsens verändert. `handleConnect()` liest und prüft zunächst lokale Rohfelder, Protokollname MQTT, Level 4, Flags, Feldgrenzen, Endbytes und UTF-8. Andere Level werden mit CONNACK 0x01 und Close abgewiesen. Authentifizierung und erfolgreiche CONNACK-Übernahme gehen der Session-/Will-Aktivierung voraus. Bestehende Authentifizierungsregeln und lokale Größenlimits bleiben bestehen; überlange Felder werden nicht still gekürzt. | MQTT-1.5.3-1/-2/-3; MQTT-3.1.2-1/-2/-3/-8/-11/-14/-15/-22; MQTT-3.1.3-1/-8/-9; MQTT-3.1.4-1 |
| SUBSCRIBE | Beim ersten Parsen wurden bereits einzelne Filter übernommen; 0x04 wurde als noLocal gelesen. Der erste Durchlauf prüft das gesamte Paket einschließlich ID != 0, mindestens einem Paar, UTF-8, Filtersyntax und Optionsbyte ausschließlich 0/1/2. Erst danach läuft die bisherige Subscription-Verarbeitung; QoS speichern/aktualisieren bleibt erhalten, noLocal bleibt false. | MQTT-2.3.1-1; MQTT-3.8.3-1/-3; MQTT-3-8.3-4 (Bezeichner im OASIS-Dokument) |

Geänderte vollständige Dateien: `src/ESPAsyncMQTTBroker.cpp`,
`src/ESPAsyncMQTTBroker.h`, `library.json`, `library.properties`, `README.md`,
`CHANGELOG.md`, `agent_worklog.md`. Version in Quelle und Metadaten: 2.0.222.

## Lokale Prüfungen

Die C++-Tests kompilieren den tatsächlichen Bibliotheksquellcode mit kleinen
Arduino-/AsyncTCP-Testdoubles. Sie laufen außerhalb des Repositorys unter
`C:\Temp\ESPAsyncMQTTBroker-222-tests`. Native Laufzeittests wurden zunächst von
Windows Smart App Control blockiert. Nachdem der Benutzer Smart App Control
selbst deaktiviert hatte, lief die Test-EXE regulär erfolgreich durch.
Kein Python-Ersatzlauf wurde als C++-Laufzeitnachweis gewertet.

Compiler: MinGW GCC 5.1.0, C++14, `-D_GLIBCXX_DEBUG -Wall -Wextra`.
Ergebnis: **50 PASS, 0 FAIL**, Prozess-Exitcode 0.
Die beiden Signedness-Warnungen in `setConfig()` und `handlePublish()` betreffen
bereits in 2.0.221 vorhandene, unveränderte Ausdrücke.

| Prüfung | Ergebnis / Nachweis |
|---|---|
| Lifetime / Close | PASS: synchroner und verzögerter Disconnect, persistente Sessionverschiebung, Callback-Close, Abbruch vor Folgepaketen |
| Fixed Header | PASS: ungültige Nibbles aller unterstützten Typen, reservierte/Server-Typen, PUBLISH QoS/DUP, gültiges wiederholtes PUBREL |
| Verbindungszustand | PASS: alle Client-Pakete vor CONNECT, zweites CONNECT, CONNECT + SUBSCRIBE + PINGREQ, fragmentierter und gebündelter Empfang |
| CONNECT | PASS: Name/Level/Flags, Will-Kombinationen, Authentifizierungsmodi, abgelehnte Verbindung ohne Will/Sessionverbrauch, alle Feldabbrüche, Endbytes, UTF-8/NUL/BOM, binärer/leerer Will, Größenlimit, kurze CONNACK-Schreibübernahme |
| SUBSCRIBE | PASS: QoS 0/1/2, ID 0, leere/abgeschnittene Payload, reservierte Bits inklusive 0x04, ungültiges UTF-8/Filtersyntax, keine Teiländerungen, QoS-Update und geordnete Mehrfilter-SUBACK |
| Lokale Zustellung | PASS: Live-QoS 1, Subscription-QoS 0-Begrenzung, PUBACK-State, excludeClientId, Retained-QoS 1/0 nach Reconnect und Löschen der Retained-Nachricht |
| ESP32-Build | PASS: PlatformIO, espressif32 6.13.0, esp32doit-devkit-v1, Arduino 2.0.17, AsyncTCP 3.5.0; Abhängigkeitsgraph zeigt ESPAsyncMQTTBroker 2.0.222 |
| Build-Größe | RAM 23.020 / 327.680 Bytes; Flash 369.641 / 1.310.720 Bytes |
| Quellgleichheit | SHA-256 der gebauten .cpp/.h-Kopien stimmt mit den Repository-Dateien überein |
| Diffprüfung | `git diff --check` ohne Fehler |

Protokolle außerhalb des Repositorys: `compile.log`, `runtime.log`,
`esp32-final-build.log`. Testdateien, Test-EXE und Firmware-Buildartefakte werden
weder committed noch in das Änderungs-ZIP aufgenommen.

## Hardwarestatus und Rückwärtskompatibilität

**Für 2.0.222 keine Hardwaretests ausgeführt.** Die hardwaregetestete Basis
2.0.221 bleibt die Vergleichsgrundlage. Simulierte MQTT-Level-4-Client-IDs im
Hosttest bestätigen keine Gerätekonformität.

| Bereich | Hardwarestatus für 2.0.222 |
|---|---|
| Parip99 .99 / Fingerprint-Kompatibilität | OFFEN |
| gumi77 .77 | OFFEN |
| Shelly .127 / ursprüngliches Produktivskript | OFFEN |
| Output 1, Output 2, Output 27 / 500-ms-Pulse | OFFEN |
| QoS 0, QoS 1, PUBACK | OFFEN; lokale Tests PASS |
| Retained QoS 1, Retained QoS 0, Reconnect, Retained löschen | OFFEN; lokale Tests PASS |
| excludeClientId | OFFEN; lokaler Test PASS |
| PINGREQ/PINGRESP | OFFEN; lokaler Test PASS |
| Keine Schleife / keine Doppelzustellung | OFFEN; kein Hardware-PASS abgeleitet |

Für die Übernahme bleiben JSON, Topics, Fingerprint-Anwendungslogik, Shelly-
Skript, `excludeClientId` und `ignoreLoopDeliver` funktional unverändert.
Nach der Hardwareprüfung den normalen Subscription-QoS 0 und das ursprüngliche
Shelly-Skript verwenden und eventuelle Retained-Testnachrichten löschen.

## Bewusst offene Punkte

Kein Topic-Matcher-Umbau, keine `$`-Wildcard-Korrektur, keine QoS-Auswahl über
überlappende Subscriptions, keine Live-RETAIN-Korrektur, keine neue Retained-
Auswahl bei mehreren Filtern, keine vollständig neuen persistenten Sessions,
keine Offline-QoS-Queue und kein vollständiger QoS-2-Umbau.
Große SUBACK-Remaining-Length, allgemeine Binärpayload-Weiterleitung, globale
UTF-8-/Paketlängenprüfung und Task-Synchronisierung bleiben weitere offene
MQTT-3.1.1-Punkte. MQTT 5 und noLocal werden nicht implementiert.

## Bereitstellung

Der Benutzer hat nach Abschluss der lokalen Laufzeittests ausdrücklich Commit
und GitHub-Push von Version 2.0.222 autorisiert; diese neuere Freigabe ersetzt
das frühere Verbot. Vorgesehen sind ausschließlich die oben genannten sieben
Dateien. Die Hardwarebestätigung bleibt ausdrücklich offen.

Zusätzliches Änderungs-ZIP:
`D:\!FingerPrint\ESPAsyncMQTTBroker_2.0.222_geaenderte_Dateien.zip`.
Es enthält ausschließlich die sieben geänderten vollständigen Dateien mit
erhaltener Ordnerstruktur; keine Tests, Patcher oder temporären Dateien.
