# Zugriff auf Service & Wartung

Steuert den Wartungskanal des Geräts: **Dateizugriff, Fernkonsole und Firmware-Update knxOTA** (über KNX-Bus / IP-Tunnel).

- **Immer erlaubt** – alle Funktionen offen. Nur in geschützten, vertrauten Netzen sinnvoll.
- **Mit Passwort** – Zugriff erst nach Anmeldung mit Passwort. Lesen/Auflisten bleibt erlaubt; Schreiben, Konsole und Update erst nach Login.
- **Im Prog-Modus** – Schreiben, Konsole und Update funktionieren nur bei aktivem Programmiermodus (Taste am Gerät); sonst gesperrt. Lesen bleibt erlaubt.
- **Blockiert** – der komplette Wartungskanal ist blockiert, **auch das Lesen** (kein Dateizugriff, keine Fernkonsole, kein knxOTA Firmware-Update). Nur die reine Geräte-Erkennung antwortet noch.

⚠ Grober Schutz gegen versehentliche oder unbefugte Wartung – **kein Ersatz für KNX Secure**.
