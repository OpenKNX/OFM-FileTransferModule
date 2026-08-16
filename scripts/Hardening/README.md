# FTC / FTM / Konsole — Härtung

Adversariale Testsuite für den **FTC-Server** (Objekt 159), den **Konsolen-Tunnel** (Objekt 160) und
den **ftc-Client**. Ergänzt die vorhandenen `../Test-Ftc*.ps1` — sie ersetzt sie nicht.

| | Frage |
|---|---|
| `../Test-FtcSuite.ps1`, `../Test-FtcStress.ps1`, `../Test-FtcResume.ps1`, `../Test-FtcConsoleUX.ps1` | **Funktioniert es?** |
| `Hardening/` (dieser Ordner) | **Was passiert, wenn die Eingabe falsch, verdreht, zu groß, unautorisiert oder zu schnell ist?** |

---

## Kein KNX-Standard — die eigene Spezifikation ist die Referenz

Für FTC gibt es **keine KNX-Prüfvorschrift**. Das Protokoll ist OpenKNX-eigen: ein
`A_FunctionProperty_Command` auf zwei Interface-Objekten. „Nach Protokoll" heißt hier also gegen
`../../doc/FTC-Reference.md`, `../../doc/errorcodes.txt`, `../../doc/FTC-Console.md` und
`../../doc/FTC-Security.md`.

> **Offener Punkt (F0 im Testplan):** Diese Dokumente haben **keine stabilen Klauselnummern**. Solange das
> so ist, kann eine Testfall-Referenz nur den Abschnittsnamen nennen statt einer Klausel — und ein FAIL ist
> schwerer zu widerlegen. Die Durchnummerierung von `FTC-Reference.md` ist die erste Aufgabe, bevor diese
> Suite ihren vollen Wert hat.

Der **Träger** ist dagegen sehr wohl KNX-spezifiziert (`03_03_07` Application Layer, geprüft in
`08_03_07`). Wo eine Erwartung daraus folgt, ist sie so zitiert.

---

## Schnellstart

```bash
# Bibliothek prüfen - ohne Gerät
pwsh scripts/Hardening/Invoke-FtmHardening.ps1 -SelfTest

# Sicherer Lauf gegen ein Zielgerät
pwsh scripts/Hardening/Invoke-FtmHardening.ps1 -Port /dev/cu.usbmodem84101 -Target 5.0.3

# Nur die Antwortmatrix, über alle drei Laufwerke
pwsh scripts/Hardening/Invoke-FtmHardening.ps1 -Port COM5 -Target 5.0.3 -Suite Response -Drive LittleFS -Drive sd

# Voller Lauf inkl. der destruktiven Fälle (kann das Gerät neu starten)
pwsh scripts/Hardening/Invoke-FtmHardening.ps1 -Port COM5 -Target 5.0.3 -Security -IncludeDestructive
```

`-Port` ist die serielle Konsole des **treibenden** Geräts (des ftc-Clients), nicht des Ziels.

---

## Suiten

| ID | Suite | Inhalt |
|---|---|---|
| `F-P` | Protokoll | Pfad an/über der Grenze, ohne NUL, Traversal, unbekanntes Laufwerkspräfix, unbekannte Kommandos, Chunk-Grenze, nicht existierende Quelle, Burst |
| `F-R` | Antwortmatrix | Kommando × Antwort × Konsument × Laufwerk — der Kern: `FileInfo` `0x00`/`0x01`/`0x02`/`0x42` gegen **alle sechs** Konsumenten |
| `F-S` | Zustandsautomat | Kommandos in falscher Reihenfolge, `Cancel` in jeder Phase, zweiter Transfer, Handle-Leck über 10 Zyklen |
| `F-A` | Zugriffsschutz | Jedes Schreibkommando in jeder Stufe ohne Autorisierung — **Nachweis am Dateisystem** |
| `F-C` | Konsole | Zeile > 1 APDU, APDU-Grenze exakt, Ring-Überlauf, Drain ohne Sitzung, Steuercodes, Koexistenz |
| `F-L` | Grenzen | Space-Guard, Durchsatz-Decke, Chunk-Grenze, fehlendes Laufwerk, `fast` auf geflutetem Bus, Flood-Cliff |
| `F-N` | Nicht-blockierend | `loop()`-Jitter während CRC, Transfer, Konsolen-Drain und Listing — als **Messung mit Schwelle** |

---

## Die zwei Regeln, die diese Suite prägen

**1. Ein korrekter Fehlercode ist kein Bestehen.**

Der Zugriffsschutz wird gegen das **Dateisystem** geprüft, nicht gegen den Rückgabewert. Ein Server, der
`0xA0 auth required` antwortet und die Datei trotzdem schreibt, ist der schlimmste denkbare Fall — und für
einen Test, der nur die Antwort liest, vollständig unsichtbar. Jeder Schreib-Fall in `F-A` fragt danach ab,
ob sich die Zieldatei geändert hat.

**2. Jede geänderte Antwort erzwingt die volle Matrix.**

`F-R` existiert wegen eines konkreten Vorfalls: Ein Server-Update führte den Status `0x02` ein
(„Größe bekannt, CRC rechnet noch, erneut fragen") und passte **einen** Client-Pfad an. Fünf
Geschwister-Zustände akzeptierten weiter nur `0x00`/`0x01` und brachen auf LittleFS: `ll` meldete 0 Byte,
`info` meldete „nicht gefunden", `apply` lief in ein Race, Downloads blieben unverifiziert. Das Review hatte
die geänderten Zeilen verfolgt statt aller Konsumenten des geänderten Werts.

Deshalb prüft `F-R` das volle Kreuzprodukt — und **die Laufwerke sind nicht austauschbar**:

| Laufwerk | FileInfo-Verhalten |
|---|---|
| LittleFS | antwortet **immer zuerst `0x02`**, für jede Datei (kooperative CRC über mehrere `loop()`-Durchläufe) |
| SD / EFC | `0x01` (nur Größe) bzw. `0x02` → `0x00`, je nach CRC-Flag |

Ein Lauf über nur ein Laufwerk beweist über die anderen fast nichts.

---

## Destruktive Fälle

`-IncludeDestructive` schaltet frei, was das Gerät wirklich belastet:

- Dateisystem füllen, Space-Guard auslösen
- echte Transfers, `Cancel` mitten drin, 10 Öffnen/Abbrechen-Zyklen
- **`F-L-7` Flood-Cliff** — jenseits von ~450 B/s ist ein Reboot des RP2040 **dokumentiertes Verhalten**.
  Der Fall behauptet nicht, dass kein Reboot passiert; er prüft, dass das Gerät **zurückkommt** und das
  Dateisystem intakt ist. Nur auf einem Laborgerät fahren.

Ohne den Schalter melden diese Fälle `SKIP` mit Grund.

---

## Was auf dem Client-Gerät bereitliegen muss

Einige Fälle brauchen vorbereitete Quelldateien auf dem treibenden Gerät:

| Datei | Wofür | Größe |
|---|---|---|
| `ftm-perf.bin` | Durchsatz, Nicht-Blockieren, `fast` unter Last, Flood-Cliff | ~200–500 KB |
| `ftm-oversize.bin` | Space-Guard, Chunk-Grenze | größer als der freie Platz des Ziels |
| `ftm-state-probe.bin` | Zustandsautomat, Abbruchzyklen | ~50 KB |

Fehlt eine davon, meldet der betroffene Fall `SKIP` mit genau diesem Grund — er behauptet nie ein Bestehen.

Für `F-L-6` (`fast` auf geflutetem Bus) wird zusätzlich ein **drittes Gerät** gebraucht, das den Bus
während des Laufs flutet. Dieser Fall schließt einen offenen Punkt: der Stall-Deadline-Re-Arm-Fix ist
gebaut, aber auf einem echt belasteten Bus noch **nicht bestätigt**.

---

## Urteile und Report

`PASS` / `FAIL` / `SKIP(Grund)` / `N-A(Grund)`, Markdown + JSON unter `Reports/`, Exit-Code ≠ 0 bei jedem
`FAIL`. Format und Engine sind absichtlich identisch mit der KNXnet/IP-Konformitätssuite im IP-Interface,
damit beide Läufe eine vergleichbare Artefaktmenge ergeben.

Die JSON-Datei erlaubt den Vergleich über Firmware-Stände hinweg — das ist der eigentliche Wert über die
Zeit, nicht der einzelne Lauf.
