# GAME_NOTES - Spielbeobachtungen (unveraenderter Spielcode)

Kurz halten. Nur belegte Beobachtungen aus Laeufen des natuerlichen Boots.

## Boot-Szenenfolge (Port, Host-Frames bei ~59,5 Hz)

| Zeit | Szene | Beleg |
|---|---|---|
| 0-10 s | Film-/Fade-Bild (grau, viele Farben); ein eigenes Level-5-Logo war nicht zu sehen | `scratch/m6b/evidence/film_10s.png` |
| 20-30 s | schwarz (Filmeende/Uebergang) | `scratch/m6b/runC_001200.bmp` |
| 40-110 s | gerenderte 3D-Szenen mit Fades (Kamera/Szene wechselt); 100-110 s blau-gruene Szene | `scratch/m6b/evidence/szene_50s.png`, `szene_100s.png` |
| ab ~110 s | **Titelbildschirm** (Logo-Band oben, Panel unten; je Lauf 110-120 s); Bildwerte nahe der Referenz: top mean (59,100,96) vs. `reports/azahar_topscreen.png` (56,92,88), bottom (17,103,89) vs. (14,80,69) | `scratch/m6b/evidence/titel_120s.png` |
| ab ~130 s | **Hauptmenue** (Buchseiten-Layout, vier Knoepfe unten), stabil bis 200 s | `scratch/m6b/evidence/hauptmenue_135s.png` |

Der Titel erscheint ohne Eingabe; die Gast-Framerate liegt in dieser Phase bei
~11 fps (Host ~60 Hz), die Szenen brauchen daher Wandzeit. Mit gehaltenem
Start/A bleibt der Titel stehen (Lauf `runB.log`, siehe unten).

## Eingabe

* **Touch auf dem unteren Screen** loest den Uebergang Titel -> Hauptmenue
  aus (`--press touch@7300:20`, Lauf `scratch/m6b/runC.log`: Titel bei 120 s,
  Uebergangsbild bei 125 s, Menue ab 130 s).
* **Start und A allein genuegen nicht** (Lauf `runB.log`):
  `start@7000:360` (6 s) und `a@7400:360` (6 s) halten den Titel bis 145 s
  unveraendert (top mean (59,100,96) identisch zum Titelframe).
* **Das Menue reagiert auf Touch**: `touch@8100:30:67:180` (zweiter Kasten
  von links in der unteren Knopfreihe) aendert danach den
  Seiteninhalt des unteren Screens dauerhaft (nur y 51..148 des unteren
  Screens, top unveraendert; 7461 Pixel heller, 5117 dunkler, also anderer
  Inhalt statt Fade). Beleg `scratch/m6b/evidence/menue_nach_touch_140s.png`
  gegen `hauptmenue_135s.png`, `scratch/m6b/evidence/menue_touch_diff.txt`.
* **Die uebrigen drei Kaesten (x=24/155/267, y=180) zeigen keine sichtbare
  Reaktion** (Lauf `runD.log`, je 30 Frames gedrueckt, Shots alle 5 s bis
  200 s: unterer Screen konstant mean (156,142,111);
  `scratch/m6b/evidence/knopfreihe.txt`).
* Der HID-Pfad ist in Ordnung, kein Workaround: `HLE_HID_LOG=1` zeigt bei
  gedrueckter Taste `state=00000001` und beim Touch
  `valid=1 pos=(67,180)`; das Spiel liest die Ringe (sonst waere keine
  Reaktion sichtbar). Layout/Takt gemaess `docs/REFERENCE.md` (HID) und
  Azahar `hid/hid.cpp`.

## Offen (Bildauffaelligkeit, keine Diagnose)

Die Seitenflaeche des Menues erscheint als sehr regelmaessiges Zellenraster
(Zellbreite ~22 px mit Randspalte). Ob das eine Textur (Papier/Kasten) oder
ein noch falsch gesampeltes/dekodiertes UI-Texturformat ist, ist nicht
geprueft; fuer den Beleg "Menue erreicht und bedienbar" ist es unerheblich.
