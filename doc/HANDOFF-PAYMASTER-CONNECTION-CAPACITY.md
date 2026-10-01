# KI-Übergabe: Paymaster-Verbindungskapazität und robuster Betrieb

Stand: 26.09.2026. Versionsneutrale Problembeschreibung für DigiByte-Paymaster-RC2.
Ausgangspunkt ist ein beobachteter Fehler in einer anderen RC2-Umgebung.
**Ob und in welcher Form die Zielversion betroffen ist, muss dort eigenständig
geprüft werden.** Gleiche RC2-Bezeichnung bedeutet weder gleichen Code noch
identische RPCs, Limits oder Testergebnisse. Dieses Dokument ist keine
Produktionsfreigabe und keine Garantie störungsfreien Netzbetriebs.

## 1. Arbeitsauftrag und Ziel

Prüfe die vorliegende RC2-Version auf die beschriebenen Fehlerklassen und
entwirf bei Bedarf eine robuste Verbindungskapazitätsverwaltung für DigiByte
Paymaster auf Client- und Providerseite. Normale Peers dürfen die für Paymaster
vorgesehene Kapazität nicht vollständig verbrauchen. Zugleich dürfen Paymaster
und Angreifer weder Blockrelay noch den Node durch unbeschränkte Verbindungen,
Warteschlangen oder Handshakes verdrängen.

Gewünschtes Verhalten: unter definierter Normal- und Spitzenlast zuverlässig
Fortschritt; bei Überlast oder Netzstörung begrenzte, klar diagnostizierbare
Ablehnung/Wartezustände. Nie Doppelzahlung, zusätzliche Signatur ohne Zustimmung
oder Freigabe möglicherweise signierter Inputs als Fehlerbehandlung.

Dieses Dokument autorisiert selbst keine Änderung laufender Dienste, Zahlungen,
Wallet-Reparatur, Veröffentlichung oder Deployment. Produktänderungen zunächst
in einem getrennten Arbeitsbaum entwickeln und prüfen, sofern beauftragt.
Bestehende lokale Änderungen erhalten. Kein automatischer Push/PR/Merge.
Die Zielumgebung und ihr Versionsstand sind vom Bearbeiter selbst zu bestimmen.

## 2. Zuerst die Zielversion untersuchen

Vor Änderungen den tatsächlichen Quellstand, lokale Abweichungen, Buildoptionen,
Binäridentität und wirksame Laufzeitkonfiguration der Zielumgebung dokumentieren.
Quellen, Builds und historische Testergebnisse nicht nachträglich umschreiben.
Keine Testergebnisse oder Reparaturskripte einer anderen Version übernehmen.

Folgende Funktionsbereiche im vorhandenen Code suchen; konkrete Dateinamen,
Klassen, RPCs und Zustandsbezeichnungen können abweichen:

| Bereich | Zu klärende Fragen |
|---|---|
| Verbindungsinitialisierung | Wie werden Full-Relay-, Block-Relay-, Feeler-, manuelle und Paymaster-Kapazität berechnet? |
| Systemgrenzen | Wird das konfigurierte Limit durch verfügbare File Descriptors oder andere Ressourcen reduziert? |
| Eingehende Annahme | Welche Slots sind verfügbar, wann wird verworfen oder ein anderer Peer verdrängt? |
| Ausgehender Verbindungsaufbau | Eigene Paymaster-Reserve oder gemeinsamer Permit-Vorrat? Werden laufende Aufbauversuche mitgezählt? |
| Direct-Protokoll | Wann ist eine Verbindung als Paymaster klassifiziert, wie werden v2 und Privacy geprüft? |
| Capacity-/Quote-Ablauf | Wann werden Inputs reserviert, Anfragen gespeichert und Fristen gestartet? |
| Wallet-/Sessionpersistenz | Wie werden signierte, unsignierte und unklare Zustände unterschieden und wiederaufgenommen? |
| Ressourcenverwaltung | Welche Queue-, Größen-, Gruppen- und Ratenlimits existieren bereits? |

Im Folgenden sind Bezeichner wie `maxconnections`, `INPUTS_RESERVED`,
`connection_pending` oder `abandon_unsigned` Beispiele bzw. Suchhinweise,
keine zugesicherte API der Zielversion. Tatsächliche Schnittstellen anhand
ihres Codes und ihrer Hilfe prüfen. Bereits gelöste Teilprobleme mit Beleg als
solche markieren; nicht durch unnötige Parallelmechanismen ersetzen.

## 3. Ausgangsbeobachtung und zu prüfende Fehlerklassen

### 3.1 Provider: keine eingehende Kapazität bei niedrigem Limit

In der Ausgangsbeobachtung wurden Testnodes mit `maxconnections=16` gestartet.
Bei folgender beispielhafter interner Berechnung blieb keine eingehende
Kapazität übrig. **Diese Formel und Konstanten sind nicht für jede RC2-Version
vorauszusetzen**, sondern gegen deren tatsächliche Slotverwaltung abzugleichen:

```text
F = min(16, maxconnections)
B = min(2, maxconnections - F)
R = F + B + 1                         # einschließlich Feeler
eingehendes Budget = maxconnections - R
gemeinsame ausgehende Permits = min(R, maxconnections)
```

Rechenbeispiele für genau dieses Modell, ohne zusätzliche Systembegrenzung:

| maxconnections | F / B / Feeler | Rechnerisches eingehendes Budget | Ausgehende Permits |
|---:|---:|---:|---:|
| 16 | 16 / 0 / 1 | −1, praktisch keine Annahme bei leerem Inbound-Bestand | 16 |
| 32 | 16 / 2 / 1 | 13 | 19 |
| 125 | 16 / 2 / 1 | 106 | 19 |

In diesem Modell entstehen fehlende Slots durch die Berechnung, nicht erst
durch 16 real verbundene Peers. Im Ausgangsfall wurde bei fehlendem
Eviction-Kandidaten eine TCP-Verbindung schon vor dem Protokollhandshake
geschlossen; bei erhöhtem Limit gelang ein eingehender v2-Handshake.
Die Zielversion braucht eine eigene Reproduktion mit ihren tatsächlichen
Grenzwerten. Ein nichtnegativer Zähler allein reicht als Fix nicht, wenn
weiterhin kein nutzbarer eingehender Slot existiert.

Ein Provider kann daher über seine ausgehenden Peers synchronisieren, lokal
konfiguriert/gestartet sein und ggf. Angebote verbreiten, während direkte
Kundenverbindungen scheitern. Lokaler Startstatus beweist keine Erreichbarkeit.
Das grundsätzliche Accept-Problem betrifft Clearnet und Onion gleichermaßen;
eine Routerfreigabe oder erfolgreicher Tor-Bootstrap behebt es nicht.

### 3.2 Client: Obergrenze ist keine Reservierung

Im untersuchten Ausgangsdesign war eine ausgehende Paymaster-Verbindung pro
Node erlaubt, sie benötigte jedoch zusätzlich ein Permit aus dem gemeinsamen
Outbound-Vorrat. Die Obergrenze reservierte also keinen Slot. Prüfen, ob die
Zielversion dieses Design oder bereits eine getrennte Reserve verwendet und
wie sich mehrere Wallets im selben Prozess die Kapazität teilen.

Bei voller gemeinsamer Belegung kann die direkte Paymaster-Verbindung nicht
aufgebaut werden. Selbst ein deutlich höheres Gesamtlimit muss diesen Vorrat
nicht vergrößern: Im obigen Rechenmodell ergeben 32 und 125 jeweils 19 Permits.
Der Spielraum jenseits von 16+2 gewöhnlichen Outbound-Peers wird u.a. auch für
Feeler/zusätzliche Peers genutzt.
Er ist dann nicht exklusiv für Paymaster. Eine gewöhnliche manuelle
Peer-Verbindung ist kein Ersatz für reservierte Direct-Kapazität. Ob ein
manueller Verbindungsmechanismus getrennte Limits besitzt, ist versionsabhängig.

**Nachweisgrenze:** Dies ist ein aus einem anderen Stand bekanntes Designrisiko,
kein Nachweis eines Fehlers der Zielversion. Dort sind gemeinsame bzw. getrennte
Permit-Verwendung und Sättigung/Fairness eigenständig zu prüfen. Häufigkeit und
Dauer möglicher Blockaden nicht ohne Messung als Produktionsausfall ausgeben.

### 3.3 Auswirkungen auf Zahlungszustand und Diagnose

Beobachtetes Symptombild: Quote-Timeout ohne erfolgreiche direkte
Onion-v2-Verbindung; anschließend eine persistente Session mit reservierten
Inputs, aber ohne nachgewiesene Signatur oder Zahlungs-TXID. Kein nachgewiesener
Geldabfluss, jedoch gebundene Inputs und fehlender Fortschritt. Davon sind
parserseitig abgewiesene Aufrufe ohne Session ebenso zu unterscheiden wie
bereits signierte oder hinsichtlich Signatur/Broadcast unklare Vorgänge.

Prüfen, ob eine gespeicherte Capacity-Anfrage abläuft und welche Wirkung
erneutes Polling tatsächlich hat. Keine stillschweigende Erneuerung annehmen.
Ein Timeout ist weder sichere Stornierung noch Erlaubnis für eine
Ersatztransaktion. Bei möglicherweise signierten Sessions gelten strengere
Anforderungen als bei nachweislich unsignierten Vorgängen.

Falls der Verbindungsaufbau nur bool zurückliefert, prüfen, ob verschiedene
Ursachen wie Privacy-Ablehnung, bereits belegter Direct-Slot oder fehlendes
Permit ununterscheidbar bleiben. „Aufbau angestoßen“ darf nicht als erfolgreich
ausgehandelter, zahlungsbereiter Kanal interpretiert werden.

## 4. Sofortmaßnahmen und ihre Grenzen

- Ein ausreichend großes Gesamtlimit kann einen rechnerischen Inbound-Mangel
  beheben. Den erforderlichen Wert aus der Zielversion und ihren Ressourcen
  ableiten; weder 32 noch 125 pauschal als Produktionskonfiguration empfehlen.
- Tatsächliche Direct-Erreichbarkeit und v2 prüfen. Gewöhnliche Onion-
  Erreichbarkeit, Tor-Bootstrap und offene Ports allein reichen nicht.
- Quote-Fortschritt bereinigt erfassen und Ablauf konkret melden, statt
  unklare Timeouts durch immer neue Zahlungsversuche zu umgehen.
- Bestehende festhängende Sessions zunächst lesend prüfen. Eine ausdrücklich
  genehmigte unsigned Recovery nur nach den Zustandsregeln der Zielversion
  ausführen, mit Backups und eindeutigem Signaturausschluss. Keine fremden
  Reparaturskripte, Request-IDs oder Walletzustände übernehmen.

Ein höheres Limit ersetzt keine echte Direct-Reserve und keinen Überlastschutz.
Eine gewöhnliche Vorprüfung beweist keine dauerhaft verfügbare Kapazität und
kann Privacy-Anforderungen berühren. Einzelne erfolgreiche Zahlungen belegen
weder Lastfestigkeit noch vollständige Tor-Isolation oder Produktionsreife.
Testwrapper dürfen nicht unbemerkt andere Connection-Limits erzwingen als der
zu prüfende Betrieb; die tatsächlichen Startargumente kontrollieren.

## 5. Lösungsansätze für den Produktcode

### A. Kurzfristig: Konfiguration validieren und Zustände verständlich machen

- Tatsächlich wirksame Limits nach FD-/Systembegrenzung prüfen, nicht nur den
  eingetragenen Konfigurationswert. Unmögliche Providerkonfiguration beim
  Aktivieren/Starten konkret ablehnen, ohne deshalb die allgemeine Node-Nutzung
  zu verbieten. Client-only, Provider und Relaybetrieb unterscheiden.
- `enabled`, `running`, fachliche Poolbereitschaft, lokale Netzwerkbereitschaft
  und extern geprüfte Erreichbarkeit getrennt ausweisen. Ohne externe Prüfung
  keine garantierte öffentliche Erreichbarkeit behaupten.
- Kleine Limits nicht stillschweigend erhöhen; Betreiber über Bedarf und
  wirksame Grenzen informieren. Ausgehende Blockchain-Konnektivität und
  Anti-Eclipse-Eigenschaften nicht beiläufig reduzieren.

Dies ist ein sinnvoller erster Schutz. Falls ein gemeinsamer Outbound-Engpass
besteht, löst Konfigurationsvalidierung allein ihn jedoch nicht.

### B. Client: echte eigene Outbound-Reserve – bevorzugter Kernansatz

Falls noch keine gleichwertige Reserve existiert, eine begrenzte
Paymaster-Permit-Klasse im Connection Manager einführen, z.B.
einen eigenen RAII-Slot für ausgehende Direct-Verbindungen. Ein Slot pro Node
ist ein konservativer Anfang für serielle Verarbeitung, keine Lastzusage.
Mehr Parallelität erst nach Prüfung von Session-, Queue- und Privacy-Isolation.

Verbindliche Anforderungen:

- Normale Full-/Block-Relay-, Feeler- und zusätzliche Verbindungen dürfen diese
  Reserve nicht belegen. Bloß einen gemeinsamen Permit-Vorrat vergrößern genügt nicht.
- Kapazität in das gesamte Verbindungs-/FD-/Speicherbudget einrechnen; nicht
  still zusätzlich unbegrenzte Sockets öffnen. Vorhandene dokumentierte
  Ausnahmen für manuelle Peers separat berücksichtigen und sichtbar machen.
- Belegung atomar für wartende/aufbauende/aktive Verbindungen verwalten.
  Bloßes Zählen aktiver Peers plus spätere Akquise ist kein ausreichender
  Nachweis für korrekte konkurrierende Connect-Aufrufe; gezielt testen.
- Alle Fehler-/Abbruch-/Shutdownpfade geben genau einmal frei. Ein RPC-Ende
  darf nicht blind eine noch genutzte Verbindung schließen; umgekehrt darf
  Warten auf Blockbestätigung nicht unnötig den einzigen Direct-Slot festhalten.
- Begrenzte faire Warteschlange für Wallets/Sessions, begrenzte Connect-/Idle-
  Zeiten, keine Verdrängung laufender Zahlungen durch neue Requests. Recovery
  darf unter Last nicht dauerhaft verhungern.
- Netzwerk-Slot und Wallet-Inputreservierung strikt unterscheiden. Socket-
  Freigabe oder Prozessneustart darf keine finanzielle Reservierung freigeben.

Eine dauerhafte TCP-Verbindung zu einem beliebigen Anbieter ist dafür weder
notwendig noch ein Ersatz. Reserviert wird Kapazität für bedarfsgesteuerte,
isolierte Verbindungen; keine heimliche gemeinsame Tor-Verbindung über Wallets.

### C. Provider: geschützte eingehende Kapazität mit Admission-Schutz

Ein zusätzlicher Client-Outbound-Slot allein löst einen Provider-Inbound-Engpass
nicht. Bei einem gemeinsamen TCP-Listener ist die Paymaster-Rolle zunächst
unbekannt; den tatsächlichen Zeitpunkt ihrer Erkennung in der Zielversion
prüfen, beispielsweise während einer Direct-Capability-Aushandlung nach v2.
BIP324 schützt den Transport, authentifiziert allein
aber weder einen vertrauenswürdigen Kunden noch eine Sponsorberechtigung.
Ein behauptetes Direct-Capability-Bit darf keine unbeschränkten Privilegien geben.

Zwei Architekturvarianten prüfen und Entscheidung begründen:

1. **Eigener Direct-Listener/Onion-Eingang mit begrenztem Admission-Budget.**
   Normale P2P-Verbindungen und Direct-Zulassung sauber trennen; Konfiguration,
   angekündigter Endpunkt und Rückwärtskompatibilität klären. Eine zusätzliche
   Portnummer allein reicht nicht, solange intern derselbe ungetrennte Pool gilt.
2. **Gemeinsamer Listener mit begrenzter Vorhandshake-Reserve und atomarer
   Umklassifizierung.** Weniger Endpunktänderungen, aber vor der Klassifizierung
   können Angreifer die Reserve ebenfalls beanspruchen. Kein uneingeschränkter
   Schutz legitimer Kunden gegen beliebige Angriffe versprechbar.

Für beide Varianten: harte Grenzen für unklassifizierte und aktive Direct-
Peers, kurze begrenzte Handshake-/Idle-Zeiten, globale und geeignete Gruppen-
Limits, begrenzte Bytes/CPU/Queues, genaues Accounting bei Beförderung/Disconnect.
Bei Onion nicht alle Benutzer als dieselbe Loopback-IP behandeln und nicht
voraussetzen, echte Client-IP-Adressen zu kennen. Bestehende Rate-Limits prüfen.

Aktive legitime Arbeit soll nicht durch normale Peer-Eviction willkürlich
abgebrochen werden. Schutz nur begrenzt und missbrauchsresistent vergeben;
keine pauschale `noban`-Freigabe und kein unbegrenzter Schutz allein aufgrund
des Direct-Bits. Normales Block-/Transaktionsrelay muss fortschreiten.

### D. Zahlungsablauf und Wiederaufnahme

- Eine lokale Direct-Kapazitätszusage möglichst vor dauerhafter Reservierung
  neuer Zahlungsinputs erwerben. Ein bloßer freier-Zähler-Check wäre TOCTOU;
  benötigte Lease atomar erwerben. Keine Walletlocks während blockierendem
  DNS-/SOCKS-/TCP-/Handshake-Warten halten.
- Reihenfolge mit bestehenden Capacity-/Quote-/Journalbindungen abgleichen;
  kein ungesicherter TOCTOU-Wechsel zwischen Netzwerk-, Pool- und Walletzustand.
  Peer-Erreichbarkeit allein garantiert weiterhin keine Quote oder Zahlung.
- Verbindungsfehler mit begrenztem Backoff/Jitter behandeln, sofern der
  jeweilige unveränderte Vorgang sicher wiederholbar ist. Lokale Wartefristen
  möglichst monoton messen; signierte/protokollierte Ablaufzeiten unverändert
  beachten. Keine unbegrenzte Verlängerung eines alten Angebots.
- Abgelaufene Anfragen mit explizitem Zustand/Aktionen zurückgeben. Keine neue
  Request-ID, Ersatztransaktion, weitere Signatur oder Budgeterneuerung als
  automatische Reparatur. Wiederholung identischer Artefakte, unsigned Abandon
  und signierte Recovery entsprechend dem Zustandsmodell der Zielversion
  auseinanderhalten; konkrete RPC-Namen und Voraussetzungen dort verifizieren.
- Unklare Schreib-/Signierantworten zuerst rekonstruieren; Timeout allein
  erlaubt weder Wiederholung noch Freigabe. Finanzielle Journale behalten.

### E. Beobachtbarkeit ohne Zahlungslecks

Strukturierte Gründe statt pauschalem bool/Timeout: kein lokaler Direct-Slot,
Queue voll, Verbindungsaufbau läuft, Proxy/Endpoint nicht erreichbar, v2-/Privacy-
Gate abgelehnt, Anfrage abgelaufen. RPC-Abwärtskompatibilität berücksichtigen.
Vorhandene geeignete Schnittstellen wiederverwenden; zusätzliche Optionen/RPCs
nur bei Bedarf entwerfen. Dieses Dokument setzt keine Reserve-Flags voraus.

Diagnosewerte: wirksame Budgets, belegte/reservierte/in-flight Slots, Queue-
Länge/Wartezeit, Ablehnungszähler, Verbindungsaufbauzeit, fortschreitende Sessions.
Keine Walletschlüssel, Capabilities, PSBTs, Empfänger oder korrelierbaren
Provider-/Zeitinformationen in normalen/teilbaren Logs. High Privacy bleibt
Onion-only mit v2 und den bestehenden SOCKS-/Isolationseigenschaften; kein
Fallback auf Clearnet, v1 oder bloß gewöhnliche Relay-Verbindungen.

## 6. Abnahme- und Regressionstests

Tests zunächst isoliert und reproduzierbar, keine Angriffe/Last gegen öffentliche
Peers. Für Sättigung kleine kontrollierte P2P-Fixtures und Manager-Tests verwenden;
nicht Dutzende vollständige Daemons starten. Bestehende Ressourcenlimits achten.

| ID | Pflichtnachweis |
|---|---|
| PC-01 | Aus der Zielversion abgeleitete Limits und Randwerte: tatsächliche Startkonfiguration, Annahme, FD-bedingte Reduktion, Fehlermeldungen; 16/32/125 nur zusätzliche Vergleichswerte |
| PC-02 | Normale Outbound-Permits vollständig belegt; trotzdem genau reservierte Direct-Kapazität verfügbar |
| PC-03 | Feeler, zusätzliche Block-/Full-Relay-Peers und Connect-Worker konkurrieren ohne Verhungern/Reserveverbrauch |
| PC-04 | Gleichzeitige Connect-Aufrufe, mehrere Wallets/Sessions; aktive plus in-flight bleiben atomar begrenzt |
| PC-05 | DNS-/SOCKS-/TCP-/v2-Fehler, Disconnect, Abbruch, Shutdown: kein Permit-Leak/Double-Release |
| PC-06 | Provider unter voller normaler Inbound-Belegung kann im definierten Lastmodell Direct-Arbeit annehmen |
| PC-07 | Handshake-Slowloris, falsches Direct-Bit, v1/unerlaubte Nachrichten, Idle-Peers: begrenzter Verbrauch und Ablehnung |
| PC-08 | Reclassification/Eviction: keine doppelte Zählung, keine unbeschränkte Bevorzugung, legitimes Relay bleibt aktiv |
| PC-09 | Keine neuen finanziellen Reservierungen bei fehlender lokaler Kapazitätszusage; Race zwischen Zusage und Reservierung |
| PC-10 | Unsigned Ablauf, verlorene Signierantwort, bereits signierte Session, Retry/Recovery: keine Doppelzahlung/falsche Freigabe |
| PC-11 | Restart, Wallet-Unload/Reload, Provider Start/Stop/Drain: richtige Lease-Freigabe, persistente finanzielle Zustände |
| PC-12 | Echte Onion-v2-Zahlung unter begrenzter Konkurrenz; Tor-Neustart/Proxyfehler, High Privacy ohne Herabstufung |
| PC-13 | USER_PAID, öffentliches/Restricted Sponsoring, separater Recoveryprovider: exakte Gebühren und Buchhaltung |
| PC-14 | Begrenzter Dauerlauf: stabile Ressourcen, faire Queue, messbarer Fortschritt, normale Blockchain-Synchronisierung |
| PC-15 | Nicht-Paymaster-Nodes, alte Gegenstellen und manuelle Peers ohne unbeabsichtigte Verhaltensänderung |

Konkurrenzfälle mit festen Seeds wiederholen; Zeitinjektion darf folgende Tests
nicht durch stehengebliebene Relay-Timer verfälschen. Vorhandene Tests passend
einbinden, nicht deren historische PASS-Ergebnisse als neue Nachweise übernehmen.
Erwartete Fallanzahl, tatsächliche Ausführung, Skips, Belege und Restlücken prüfen.
Produktänderungen erfordern einen neuen getrennten Build und neue Nachweise;
ein erfolgreicher Einzeltest oder ein fremder Testbericht genügt nicht.
Sanitizer- und Sicherheitsprüfung vor Produktionsfreigabe einplanen, nicht als
bereits bestanden ausgeben. Nicht vorhandene Modelle oder Optionen als solche
dokumentieren und mit den Anforderungen abgleichen, nicht als PASS zählen.

## 7. Umsetzung und betriebliche Einführung

1. Stand/Bedrohungsmodell und harte Ressourcenbudgets festhalten; bestätigte
   Bugs, abgeleitete Risiken und Designentscheidungen getrennt dokumentieren.
2. Fehlende Client-Outbound-Reserve mit atomarem Lifetime-Accounting,
   Fehlergründen und Sättigungs-/Konkurrenztests implementieren. Provider-Admission-Variante separat
   entscheiden; ein Client-Fix ist noch kein vollständiger Provider-Fix.
3. Provider-Inbound-Schutz, Readiness und sichere Wallet-/Transport-Verzahnung
   prüfen. Keine neue Policy darf rückwirkend eine signierte Session verwerfen.
4. Bestehende Functional-/CLI-/Tor-Tests plus obige Regressionen ausführen;
   bekannte Lücken und nicht ausgeführte Sicherheitsprüfungen offen ausweisen.
5. Deployment nur nach Freigabe, zunächst isoliert/Testnet und mit Backups.
   Offene signierte Sessions vor Wartung prüfen; Drain und regulären Shutdown
   verwenden. Kein paralleler Betrieb restaurierter Kopien derselben Identität.
   Rückweg nur bei nachgewiesener Wallet-/Journalformat-Kompatibilität, niemals
   blind ein älteres Walletbackup wieder online nehmen.

Bei bestätigtem Reserve-/Überlastproblem nicht als vollständige Lösung
akzeptieren: ausschließlich maxconnections erhöhen, gewöhnliches addnode,
laufend Nodes neustarten, automatische Ersatzrequests, pauschales Abandon,
mehr DD/DGB verlangen oder Whitelist/Netzwerkschutz global abschalten.

## 8. Ergebnisse und zentrale Befunde der Zielumgebung

Ein zentrales Befund-/ToDo-Dokument in der Zielumgebung anlegen oder das dort
vorhandene verwenden. Keine verteilten, voneinander abweichenden Fehlerlisten.
Für jeden Punkt mindestens erfassen:

```text
ID, Zielversion/Build, Einstufung (bestätigt / Hypothese / bereits gelöst),
Auswirkung und Priorität, Vorbedingungen, minimaler Reproducer,
erwartetes und tatsächliches Verhalten, betroffene Funktionsbereiche,
Lösung/Designentscheidung, Testfälle und Ergebnisse, Belege, Restaufgaben.
```

Ergebnisse als PASS, FAIL, BLOCKED, NOT_RUN oder begründet N/A ausweisen.
Historische Fehlversuche nicht überschreiben; ein Nachtest erhält einen neuen
Nachweis. Hypothesen werden erst nach Quellprüfung bzw. Reproduktion zu
bestätigten Befunden. Ein bereits korrigierter Zielstand braucht keine erneute
Implementierung desselben Fixes, wohl aber passende Regressionsabdeckung.

Private Journale, Backups, Cookies und Walletdateien sind **kein** unbereinigtes
Uploadpaket für eine externe KI. Finanzierte Wallets und persistente Sessions
vor jeder Aktion frisch prüfen. Bestehende Tages-/Zeit-/Gebührenbudgets sowie
Inputreservierungen nicht durch eine Reparatur stillschweigend zurücksetzen.

Erwartete Übergabe der nachfolgenden KI: begründete Architekturentscheidung,
gegebenenfalls separat beauftragter Patch, genaue Versions-/Buildbindung,
vollständige Testergebnisse und verbleibende Risiken, Ressourcen-/Lastgrenzen,
Betriebsanleitung sowie Migrations-/Rollback-Bedingungen. Keine pauschale Aussage
„produktionssicher“ allein aufgrund eines einzelnen erfolgreichen Zahlungstests.
