## 🔧 22.10.2026 – Retain-Merkmal kompatibel an die Anwendung weiterreichen

- Auftrag: Schaltpfade des Fingerscanners reparieren und geprüfte Quellen veröffentlichen.
- Basis mit GitHub-HEAD 2df3d0995c48bc914904d94f150b3d7eada0df48 abgeglichen. Keine fremden Bibliotheken bearbeitet.
- `src/ESPAsyncMQTTBroker.h`: zusätzliche optionale Schnittstelle `onMessageWithRetain`; bisherige Text-/Binaercallbacks bleiben erhalten.
- `src/ESPAsyncMQTTBroker.cpp`: Retain von QoS 0/1 sowie gespeicherter QoS-2-Nachricht bis zur Anwendung transportieren; Text höchstens einmal konvertieren.
- `tests/retain_callback_test.py`: Originalfunktion mit Address-/UndefinedBehaviorSanitizer geprüft: PASS für beide Retain-Werte, Legacy-/Binaercallbacks und Aufrufstellen.
- Keine Änderung von Versionszeilen oder TCP-Einstellungen. Hardwaretest nicht durchgeführt. Vollständige Firmwareintegration mit MAX/MIN/Ro folgt im Firmwareprojekt.
- Direkter Editor-Zeitachsenzugriff nicht verfügbar; Dateinachweise hier dokumentiert.
