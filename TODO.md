# TODO – ESPAsyncMQTTBroker

Stand: 05.10.2026. Diese Liste ist der Einstieg für die nächste Arbeitssitzung.

## Aktueller stabiler Stand

- [x] Version **2.0.222** umgesetzt und geprüft.
- [x] 50 lokale C++-Tests: PASS, 0 FAIL.
- [x] 55 Tests auf dem echten Broker: PASS, 0 FAIL.
- [x] Gerätepfade mit Parip99 `.99`, gumi77 `.77` und Shelly `.127` geprüft.
- [x] QoS 0/1, PUBACK, Retained QoS 1/0 nach Reconnect und Retained-Löschung geprüft.
- [x] Normalzustand wiederhergestellt: QoS 0, `DEBUG_NONE`, originales Shelly-Skript, Test-Retained gelöscht, COM-Ports frei.
- [x] Stabilen Stand mit Abschluss-Commit `72087d6` und Tag **`v2.0.222`** auf GitHub festgehalten.

**2.0.222 bleibt eingefroren.** Weitere funktionale Änderungen gehören in
eine folgende Version. Diese Liste ist keine Aufforderung, alle Restpunkte
auf einmal umzusetzen.

## Hier beim nächsten Mal beginnen

- [ ] Einen kleinen, risikoarmen Umfang aus den MQTT-3.1.1-Restpunkten unten auswählen.
- [ ] Für jeden ausgewählten Punkt Ursache, betroffene Funktionen, OASIS-Anforderung und Regressionstest festhalten.
- [ ] Fehlendes Verhalten zuerst durch einen gezielten Test nachweisen.
- [ ] Änderungen einzeln umsetzen und gegen den stabilen Stand `v2.0.222` prüfen.
- [ ] Versionsnummer erst beim tatsächlichen Beginn der neuen Implementierung pflegen.

Normative Grundlage ausschließlich:
[OASIS MQTT 3.1.1 einschließlich Approved Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html).

## MQTT-3.1.1-Restpunkte

Die Reihenfolge ist ein Arbeitsvorschlag. Die größeren Punkte bleiben eigene
Arbeitspakete und werden nicht automatisch Teil der unmittelbar nächsten Version.

### Zuerst: begrenzte Paket- und Zustellungskorrekturen untersuchen

- [ ] Weitere Paketlängen- und Packet-Identifier-Prüfungen außerhalb CONNECT/SUBSCRIBE vervollständigen; Remaining-Length-Randfälle berücksichtigen.
- [ ] Mehrbyte-Remaining-Length für große SUBACK-Antworten korrekt kodieren.
- [ ] RETAIN-Flag bei Live-Zustellung korrigieren; gespeicherte Retained-Zustellung separat regressionsprüfen.

### Danach: Topic-Filter und Auswahl der Zustellung

- [ ] Topic-Matcher-Randfälle und die `$`-Wildcard-Sonderregel prüfen und gezielt korrigieren.
- [ ] QoS-Auswahl bei überlappenden Subscriptions standardkonform prüfen; bisherige Subscription-QoS-Funktion erhalten.
- [ ] Retained-Auswahl beim SUBSCRIBE mit mehreren Filtern prüfen; unbeabsichtigte erneute Zustellung über alte Filter untersuchen.

### Größere, getrennte Arbeitspakete

- [ ] Binärpayloads einschließlich eingebetteter Nullbytes durchgängig unverändert weiterleiten; Auswirkungen auf bestehende öffentliche Text-Callbacks zuerst klären.
- [ ] UTF-8-Prüfung auf die übrigen vorgeschriebenen MQTT-Textfelder erweitern.
- [ ] Persistente Sessions vervollständigen, einschließlich Session-Löschung und Wiederaufnahme.
- [ ] Offline-QoS-Queue für persistente Sessions separat konzipieren und testen.
- [ ] QoS-2-Zustand und Wiederaufnahme vervollständigen; DUP-/PUBREL-Wiederholungen berücksichtigen.
- [ ] Ressourcenlimits und Task-Synchronisierung anhand konkreter Fehlerfälle prüfen; Besitzarchitektur nicht ohne begründeten Bedarf umbauen.

## Bei jeder folgenden Version erhalten und prüfen

- [ ] Parip99 `.99`: Brokerstart, Fingererkennung, Output 1 und Output 2.
- [ ] gumi77 `.77`: CONNECT, SUBSCRIBE QoS 0/1, Live-Zustellung, PUBACK, Reconnect, PINGREQ/PINGRESP.
- [ ] Shelly `.127`: originales Produktivskript, unveränderte FP-JSON, Output 27.
- [ ] Retained QoS 1/0 nach Reconnect und anschließende Löschung der Testnachricht.
- [ ] `excludeClientId`, keine unerwünschte Rückschleife und keine unbeabsichtigte Doppelzustellung im geprüften Pfad.
- [ ] Topics, JSON und bisherige Fingerprint-Aufrufe unverändert.
- [ ] Nach temporären Tests wieder QoS 0, `DEBUG_NONE`, originales Shelly-Skript und freie COM-Ports herstellen.

MQTT 5 und `noLocal` werden nicht eingeführt. `ignoreLoopDeliver` wird nicht
aktiviert. Der bestehende Ausschluss des ursprünglichen Publishers und
`excludeClientId` bleiben für die Rückwärtskompatibilität erhalten; eine
spätere Änderung dieses Verhaltens benötigt eine eigene Betrachtung.
Fingerprint-Anwendungslogik gehört nicht zu diesen Broker-Arbeitspaketen.

## Separat, ohne das Einfrieren zu blockieren

- [ ] Nur bei Bedarf: physische 500-ms-Pulsdauer mit geeigneter Messhardware prüfen.

`duration=500` im JSON ist nachgewiesen. HTTP-Abtastung war für einen präzisen
physikalischen Zeitnachweis nicht ausreichend. Das ist kein offener Broker-
Quellcodefix für 2.0.222.

## Nachweise wiederfinden

- Broker-Änderungs- und Prüfbericht: [agent_worklog.md](agent_worklog.md).
- Funktionsstand und offene Einschränkungen: [README.md](README.md).
- Versionshistorie: [CHANGELOG.md](CHANGELOG.md).
- Stabiler GitHub-Stand: [v2.0.222](https://github.com/parip69/ESPAsyncMQTTBroker/tree/v2.0.222).
- Ausführlicher Hardwarebericht: `D:\!FingerPrint\###1A-Parip69-Fingerprint_Original\agent_worklog.md`, Einträge vom 05.10.2026.
- Aktuelle Testprogramme und Einzelprotokolle: `C:\Temp\ESPAsyncMQTTBroker-222-tests`.

- [ ] Vor dem Aufräumen von `C:\Temp` benötigte Testprogramme und Nachweise dauerhaft sichern, damit die Prüfungen später reproduzierbar bleiben.
