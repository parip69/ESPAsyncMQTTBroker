# Changelog

## v1.5.0 – Sicherheits- und Memory-Update (21.05.2025)
### Sicherheit und Stabilität
- Smart Pointer (`std::unique_ptr`) statt Raw Pointer für bessere Speicherverwaltung
- Buffer-Überprüfungen in sämtlichen Funktionen eingebaut
- Feste Grenzwerte für Topic- und Payload-Größen definiert
- Fehlerbehandlung und Exception-Handling verbessert

### Performance und Zuverlässigkeit
- Optimiertes Speichermanagement für Langzeitstabilität
- Verbesserte MQTT 5.0 Unterstützung
- Asynchrone Verarbeitung optimiert

### Benutzerfreundlichkeit
- Umfassende Dokumentation mit Doxygen-Kommentaren
- Zentrales Logging-System mit verschiedenen Debug-Levels
- Aktualisierte README mit mehr Beispielen und Fehlerbehandlung
- Callback-basierte API für alle MQTT-Ereignisse

### Nächste geplante Schritte
- Umstellung der Client-Verwaltung auf `std::unordered_map` für O(1)-Zugriff
- Unit-Tests für Topic-Matching und andere kritische Funktionen
- Erweiterte MQTT 5.0 Funktionen (User Properties, etc.)
- Beispiel für Integration mit WebSockets für Web-UI

## v1.0.0 – Erster stabiler Release (02.04.2025)
- MQTT-Broker auf Basis von AsyncTCP
- Unterstützung für einfache Topic-Callback-Verarbeitung
- Beispiel: LED-Steuerung über Topic `/ring`
- Webinterface zur Anzeige verbundener Clients
