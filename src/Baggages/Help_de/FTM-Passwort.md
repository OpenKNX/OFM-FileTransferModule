# Passwort für den Wartungszugriff

Passwort für die Anmeldung am Wartungskanal (Dateien, Konsole, Firmware), max. 16 Zeichen.

Die Prüfung erfolgt per Challenge-Response – **das Passwort wird nie im Klartext über den Bus übertragen**.

**Hinweis:** Das Passwort ist im ETS-Projekt sichtbar und wird ohne KNX Secure einmalig im Klartext auf das Gerät geladen (auf einer unverschlüsselten Linie mitlesbar). Der laufende Betrieb ist davon nicht betroffen.

**Brute-Force-Schutz:** Nach 3 Fehlversuchen greift eine ansteigende Wartezeit (1, 2, 4, 8 … Minuten) vor jedem weiteren Versuch. Ein erfolgreicher Login setzt den Schutz zurück.
