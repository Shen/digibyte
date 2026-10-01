# Paymaster Security- und Funktionalitätsaudit

> **Historical audit record; status note added 2026-09-23.**
> This report and its findings/test counts apply to the August source and
> working tree named below. Its original wording is preserved; it is not an
> audit of `a4f17f6315`, an independent certification, or release approval.
> Current English documentation is indexed in [PAYMASTER.md](../PAYMASTER.md).
> The [hardening control map](../DIGIDOLLAR_PAYMASTER_HARDENING_PLAN.md) and
> [release gate](../doc/digidollar-paymaster-release-gate.md) cover the present
> integration, dated follow-up results and outstanding acceptance requirements.
> Read the September [workflow review](../doc/digidollar-paymaster-edge-case-review.md)
> for later findings and their recorded remediation.

**Stand:** 8. August 2026
**Auditbasis:** Branch `feature/digidollar-paymaster-v1`, Härtungsbaseline `8573b25bd9` einschließlich der anschließend verhaltensneutral modularisierten Wallet-Paymaster-Dateien
**Status:** Quellcodeaudit und gezielte dynamische Verifikation; keine formale Zertifizierung und kein Ersatz für ein unabhängiges Penetrationstest-/Release-Audit

## 1. Kurzfazit

Der Paymaster-Code besitzt für ein noch nicht produktiv eingesetztes Feature bereits eine überdurchschnittlich starke Sicherheitsbasis. Besonders positiv sind die strikte Bindung von Intent, Quote, Manifest, PSBT und finaler Transaktion, die Trennung der Signaturrollen, atomare Wallet-Transaktionen, Replay-/Equivocation-Schutz, aktuelle Formatversionen, Ressourcenlimits und die umfangreichen Negativtests.

Im ursprünglichen Audit und den anschließenden Prüfläufen wurden **keine bestätigten kritischen oder hohen Schwachstellen**, zwei Befunde mittlerer Priorität, vier niedrig priorisierte Härtungs-/Assurance-Themen und ein informativer Wartbarkeitshinweis gefunden. Die beiden mittleren Befunde sowie BIP340-Aux-Randomness, die CI-Abdeckung, ein False-Green im Functional-Test-Runner und die domänenweise Dateiaufteilung wurden im geprüften Arbeitsbaum behoben. Das At-rest-Risiko ist jetzt ausdrücklich dokumentiert; eine zusätzliche Paymaster-Feldverschlüsselung wurde wegen der Schlüssel-, Unlock-, Retry- und Recovery-Auswirkungen nicht übereilt eingeführt.

| Schweregrad | Ursprünglich | Offen nach Umsetzung |
|---|---:|---:|
| Kritisch | 0 | 0 |
| Hoch | 0 | 0 |
| Mittel | 2 | 0 |
| Niedrig | 4 | 1 dokumentiertes Restrisiko |
| Informativ | 1 | 0 |

„Nicht gefunden“ bedeutet nicht „nicht vorhanden“. Das Ergebnis gilt nur für den genannten Arbeitsbaum und die unten dokumentierten Prüfgrenzen.

## 2. Umfang und Methode

### 2.1 Geprüfte Flächen

Geprüft wurden 81 Paymaster-benannte Quell-, Test- und Dokumentationsdateien mit ungefähr 72.400 Zeilen sowie die sicherheitsrelevanten Integrationspunkte. Dazu gehören insbesondere:

- `src/paymaster/`: Protokoll, Wire-Formate, Provider/Client, Directory, Manager, PSBT, Recovery, Budget, Reputation, Sponsorship und Validierung;
- `src/wallet/paymaster*.{cpp,h}`: Wallet-Identität, Provider-Liquidität, Signaturpfade, PSBT und persistenter Zustandsautomat;
- `src/wallet/rpc/paymaster*.{cpp,h}`: RPC-Grenze, Orchestrierung, Wallet-Unlock, Provider-Automatik und Recovery;
- `src/wallet/walletdb.{cpp,h}`: Paymaster-Datensätze und Transaktionssemantik;
- `src/net.cpp`, `src/net_processing.cpp`, `src/netbase.cpp`: BIP324-v2-Isolation, Nachrichtenannahme, Limits, Tor/SOCKS und Logging;
- Paymaster-Unit-, Wallet-, P2P-, Functional- und Fuzz-Tests;
- Architektur-, Betriebs- und Sicherheitsdokumentation sowie CI-Workflows.

Nicht Bestandteil waren eine vollständige Konsensprüfung außerhalb der Paymaster-Berührungspunkte, eine kryptografische Prüfung von secp256k1 selbst, eine externe Abhängigkeits-/CVE-Datenbankabfrage, ein Live-Netz-Penetrationstest und eine formale Verifikation.

### 2.2 Bewertungsmaßstab

Die Prüfung orientiert sich, soweit auf eine native C++-Wallet und ein P2P-Protokoll übertragbar, an:

- [OWASP ASVS 5.0](https://owasp.org/www-project-application-security-verification-standard/) für Eingabevalidierung, Kryptografie, Logging, Datenschutz, Authentisierung und sichere Zustandsänderungen;
- [MITRE CWE Top 25 (2025)](https://cwe.mitre.org/top25/archive/2025/2025_cwe_top25.html) und den jeweiligen CWE-Klassen;
- [SEI CERT C++ Coding Standard](https://wiki.sei.cmu.edu/confluence/x/Wnw-BQ) für C++-Fehlerbehandlung, Integer-, Speicher-, I/O- und Nebenläufigkeitssicherheit;
- [NIST SP 800-218 SSDF 1.1](https://csrc.nist.gov/pubs/sp/800/218/final) für sichere Entwicklungs-, Prüf- und Release-Prozesse;
- [BIP340](https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki) für Schnorr-Signaturen und Auxiliary Randomness.

Dies ist keine Behauptung vollständiger ASVS-, NIST- oder ISO-Konformität. Die Standards dienen als strukturierter Prüfrahmen.

### 2.3 Schweregrad

- **Kritisch:** unmittelbarer, breit ausnutzbarer Verlust von Schlüsseln oder Geldern beziehungsweise vollständige Umgehung zentraler Autorisierung.
- **Hoch:** realistisch ausnutzbarer Verlust von Geldern, dauerhafte Signatur-/Budgetumgehung oder schwere Remote-Kompromittierung.
- **Mittel:** relevante Integritäts-, Verfügbarkeits- oder Datenschutzverletzung mit zusätzlichen Voraussetzungen oder begrenzter Reichweite.
- **Niedrig:** Härtungs-, lokale Datenschutz- oder Assurance-Lücke ohne unmittelbaren Remote-Funds-at-Risk-Pfad.
- **Informativ:** Qualitäts-/Wartbarkeitshinweis oder akzeptierbares Restrisiko.

## 3. System- und Bedrohungsmodell

### 3.1 Schutzgüter

- private Wallet- und Provider-Identitätsschlüssel;
- DD- und DGB-UTXOs sowie deren Reservierungen;
- Benutzer- und Provider-Signaturautorität;
- Service- und Netzwerkfee-Budgets;
- Sponsorship-Capabilities und deren Replay-Sperren;
- Intent-, Quote-, Manifest-, PSBT- und finale Transaktionsbindung;
- Recovery-Artefakte und historische Policy-/Chainstate-Bindungen;
- Provider-Reputation und Equivocation-Evidenz;
- Metadaten wie Provider-Endpunkt, Request-/Session-Beziehungen und zeitlicher Ablauf.

### 3.2 Relevante Angreifer

- nicht authentisierte P2P-Peers mit beliebigen, großen, alten, abgeschnittenen oder widersprüchlichen Nachrichten;
- bösartige Paymaster-Provider oder Clients;
- Replay-, Reordering-, Equivocation- und Crash/Restart-Szenarien;
- lokale Leser von `debug.log`, Wallet-Backups oder des Wallet-Datenbestands;
- alte oder beschädigte Entwicklungsdatensätze;
- Fehlkonfigurationen von Tor, Logging, Capture, Wallet-Unlock oder Provider-Budgets.

### 3.3 Vertrauensgrenzen

1. P2P-Nachricht → BIP324-v2-Transport → Wire-Decoder;
2. Wire-Artefakt → semantische Validierung → Manager-Inbox;
3. RPC/GUI → Wallet-Orchestrierung und Wallet-Unlock;
4. Wallet/Signer → rollenbeschränkte PSBT-Signatur;
5. Chainstate/Mempool → Prevout-, Konflikt- und Broadcast-Prüfung;
6. Prozesszustand → atomare Wallet-Datenbank → Restart/Recovery;
7. Paymaster → Tor/SOCKS-Proxy → Provider-Endpunkt;
8. interne Fehler → Debug-/Wallet-Logs und Support-Artefakte.

## 4. Bereits wirksame Sicherheitskontrollen

| Bereich | Bewertung | Evidenz |
|---|---|---|
| Aktuelle Formate | Stark | Wire-, Intent-, Quote-, Manifest-, Recovery- und Persistenzvalidatoren verlangen jeweils exakt `CURRENT_VERSION` beziehungsweise Protokoll V5. |
| Canonical Encoding | Stark | Netzwerkdecoder verlangen vollständigen Streamverbrauch; Autorisierungsartefakte werden vollständig dekodiert und bytegenau reserialisiert. Angefügte Bytes für Request, Response, Transaktion und PSBT werden getestet. |
| Signaturbindung | Stark | Tagged Hashes binden Genesis, Provider, Intent, Policy, Quote, Ressourcen, Zeitfenster, Template und finalen Tx-/Witness-Hash. Neue BIP340-Artefakte verwenden Auxiliary Randomness; exakte Retries laden die zuvor atomar persistierten Bytes. |
| PSBT-Rollen | Stark | USER- und PROVIDER-Eingaben werden getrennt; unbekannte Rollen/Felder, unzulässige Modifikationen und falsche Sighash-Modi werden abgelehnt. |
| Provider-Budget | Stark | Netzwerkfees werden vor Provider-Signatur atomar reserviert; pro Transaktion, Stunde, Tag, Empfängerklasse und Netgroup gelten überprüfte Grenzen. |
| Client-Fee-Budget | Stark | Servicefees sind an Commit/Manifest gebunden und besitzen monotone Zustände `RESERVED`/`SPENT`/`RELEASED`. |
| Replay/Equivocation | Stark | Request-, Session-, Nonce-, Commit- und Content-Hash-Bindungen, Tombstones und persistente Capacity-/Quote-Evidenz verhindern konfliktbehaftete Wiederholungen. |
| Recovery | Stark | Self- und Alternative-Recovery sind an Session, Inputs, Manifest, Referenzblock, Policy-Hash, PSBT und finales Artefakt gebunden. Exakte historische Retries bleiben erlaubt, neue Autorität entsteht daraus nicht. |
| Netzwerkisolation | Stark | Direkte Nachrichten benötigen BIP324 v2, richtige Richtung und aktivierte Paymaster-Aushandlung; private Sockets nehmen nicht am normalen Tx-/Dandelion-Relay teil. Raw Capture und Trace werden für Paymaster-Payloads unterdrückt. SOCKS-Credentials werden nie geloggt; private Ziele werden bis in den SOCKS-Unterbau redigiert. |
| Ressourcenbegrenzung | Stark | 1 MiB Nachrichtenlimit, 900 KiB PSBT-Limit, 60-s-TTL, Vor-Deserialisierungs-Ratelimit, Token-Buckets, begrenzte Replay-Maps und Inbox-Limits. |
| Directory | Stark | Maximal 256 Provider und vier Offers je Announcement; Sequenzen, Outpoint-Eindeutigkeit, Ablauf, Chainstate, Mempool-Konflikte und Kontrollsignaturen werden geprüft. |
| Integerarithmetik | Stark | Fee-/Betragsarithmetik ist ganzzahlig, bereichs- und überlaufgeprüft; Zeitdifferenzen verwenden saturierende/überlaufsichere Helfer. |
| Atomarität | Stark | Autorisierung, Reservierung, Budget, Commit, Resultat und Wallet-Downgrade-Flag werden in Wallet-DB-Transaktionen geschrieben; Fehler-Injection-Tests decken Teilfehler ab. |
| Persistenzintegrität | Stark | Typisierte Reads unterscheiden `FOUND`, `NOT_FOUND`, `UNSUPPORTED_VERSION` und `READ_ERROR`. Referenzierte Daten werden vor Pruning oder Abgleich validiert; ein Fehler beendet den Pfad vor jeder Mutation. |
| Capability-Minimierung | Stark | Die geheime Sponsorship-Capability wird vor Persistenz aus dem Quote Request entfernt; gespeichert wird nur ihre kryptografische Bindung. |
| Nebenläufigkeit | Gut | Directory, Manager-Ratenzustand und Direct-Inbox besitzen getrennte, annotierte Mutex-Grenzen; Wallet-Persistenz läuft unter `cs_wallet`. |

## 5. Befunde

Die Abschnitte „Ausgangsbeobachtung“ dokumentieren den Zustand vor den in diesem Arbeitsbaum vorgenommenen Änderungen. Dateilinks zeigen auf die aktuelle Umsetzung; bewusst werden keine nach Änderungen schnell veraltenden Zeilenanker verwendet.

### PM-01 – Ausgangsbefund: Mehrdeutige Persistenz-Leseergebnisse in Wartungs- und Pruningpfaden

**Schweregrad:** Mittel
**Vertrauen:** Hoch
**Klassen:** CWE-20 (Improper Input Validation), CWE-754 (Improper Check for Unusual or Exceptional Conditions)
**Umsetzungsstatus:** Behoben im geprüften Arbeitsbaum

#### Ausgangsbeobachtung

Viele ältere `WalletBatch::ReadPaymaster*`-APIs liefern nur `bool`. `false` kann „Schlüssel fehlt“, „Deserialisierung fehlgeschlagen“, „Version falsch“ oder „inhaltliche Bindung ungültig“ bedeuten. Zum Beispiel vermischten `ReadPaymasterSession` und `ReadPaymasterAttempt` in [`walletdb.cpp`](../src/wallet/walletdb.cpp) diese Fälle. Für neuere Equivocation-Datensätze existierte bereits das geeignetere `DatabaseReadStatus {FOUND, NOT_FOUND, READ_ERROR}` – der Schutz war also im Projekt grundsätzlich vorhanden, aber nicht durchgängig.

Mehrere sicherheitsrelevante Aufrufer kompensieren die Bool-Mehrdeutigkeit mit `HasPaymaster*`. Folgende Pfade tun dies jedoch nicht vollständig:

- `ReconcileProviderPoolSuccessors` in [`paymasterstore_finalization.cpp`](../src/wallet/paymasterstore_finalization.cpp) beendete sich erfolgreich, wenn `ReadPaymasterProviderPool` fehlschlug – auch wenn ein vorhandener Pool beschädigt oder veraltet war.
- `PruneFinalSession` übersprang bei einem nicht lesbaren Attempt den Datensatz und fuhr mit Tombstone/Entfernung der Session fort. Commit, UserAuthorization, Result, Outcome oder große PSBT-/Transaktionsartefakte konnten dadurch verwaisen.
- Der Client-Fee-Abgleich in `ReconcileFinalTransaction` übersprang einen nicht lesbaren Attempt und konnte eine noch reservierte Fee deshalb nicht freigeben.
- `GetSessionBySessionId` fiel nach einem fehlgeschlagenen Live-Session-Read auf einen Tombstone zurück, ohne vorher `HasPaymasterSession` zu prüfen.
- Der Provider-Finanzabgleich in [`wallet/rpc/paymaster.cpp`](../src/wallet/rpc/paymaster.cpp) behandelte nicht lesbare Attempts wie unpassende/fehlende Attempts und markierte die Historie erst indirekt als partiell.

#### Auswirkung

Ein alter oder beschädigter Datensatz wird nicht zur neuen Signaturautorität; die starke Signatur- und Transaktionsbindung verhindert einen offensichtlichen Funds-at-Risk-Pfad. Möglich sind jedoch:

- verwaiste oder länger als vorgesehen gespeicherte Autorisierungs-/Transaktionsmetadaten;
- stale Budget-/Fee-Reservierungen und unvollständige Provider-Finanzhistorie;
- erfolgreich gemeldete Wartung trotz nicht auswertbarem Provider-Pool;
- uneindeutige Operator-Diagnose und inkonsistente Current-only-Semantik;
- schwer testbare Restart-/Upgrade-Fehler.

Ein Remote-Peer kann die Wallet-Datenbank nicht unmittelbar verändern; realistische Auslöser sind alte Entwicklungswallets, beschädigte Backups, Storage-Fehler oder Softwarefehler. Das reduziert die Ausnutzbarkeit, nicht aber die Integritätsanforderung.

#### Empfehlung

1. Alle Paymaster-Lese-APIs auf einen typisierten Status umstellen: mindestens `FOUND`, `NOT_FOUND`, `UNSUPPORTED_VERSION`, `CORRUPT`/`READ_ERROR`.
2. Für Versionsfehler ausschließlich Datensatztyp, gefundene und erwartete Version ausgeben; keine IDs, Endpunkte, PSBTs oder Transaktionen.
3. Referenzierte Child-Datensätze vor Beginn einer Pruning-Transaktion vollständig laden und validieren. Bei einem Fehler keine Tombstones, Erases, Budget- oder Sessionänderungen durchführen.
4. `ReconcileProviderPoolSuccessors`, Finance-, Fee- und Recovery-Abgleich bei vorhandenen unlesbaren Datensätzen fail-closed beenden.
5. Optional eine interne referenzielle Integritätsprüfung bereitstellen, die nur Typen/Zähler meldet und keine sensitiven Inhalte ausgibt.

#### Akzeptanztests

- authentisch verkürzte Altformate und aktuelle Layouts mit abgesenkter Versionsnummer für Session, Attempt, Pool, Commit, Result, Authorization und Recovery;
- beschädigte aktuelle Datensätze mit gültigem DB-Schlüssel;
- Pruning/Abgleich muss mit sanitisiertem Fehler abbrechen und einen bytegleichen DB-Snapshot hinterlassen;
- „wirklich nicht vorhanden“ muss weiterhin idempotent behandelt werden;
- kein Tombstone und kein Child-Erase, solange ein referenzierter Datensatz nicht vollständig validiert ist.

#### Umsetzung und Verifikation

`DatabaseReadStatus` unterscheidet jetzt `FOUND`, `NOT_FOUND`, `UNSUPPORTED_VERSION` und `READ_ERROR`. Die versionierten Reader erfassen auch die Versionsnummer authentisch verkürzter Altformate, ohne beschädigte aktuelle Datensätze als Altformat auszugeben. Session- und Attempt-Zustände werden beim Lesen validiert.

Session-/Recovery-Lookup, Provider-Pool-, Client-Fee-, Provider-Finanz-, Wartungs- und Zuverlässigkeitsabgleich behandeln nur echtes `NOT_FOUND` als Abwesenheit. Auch die Coin-Selection-Schutzschicht behandelt eine unlesbare Einzelreservierung als belegt und einen unlesbaren Provider-Pool nicht als leere Eingabemenge. Wartung prüft den Pool vor Budgetreservierung und Transaktionserzeugung und überschreibt einen später unlesbaren Pool nicht. Provider-Finalisierung verlangt einen vollständig validierten aktuellen Pool, bevor Commit, Resultat oder Budgetzustand verändert werden. `PruneFinalSession` lädt und validiert Session, Indizes, Attempts, Reservierungen und alle referenzierten Artefakte vollständig, bevor die schreibende Transaktion beginnt. Fehlerdiagnosen nennen nur Datensatztyp sowie gefundene und erwartete Version.

Gezielte Tests decken vollständige und verkürzte Altformate, beschädigte aktuelle Records, unveränderte Datenbank-Snapshots nach Fehlern und erfolgreiches idempotentes Verhalten bei wirklich fehlenden Datensätzen ab.

### PM-02 – Ausgangsbefund: High-Privacy-Endpunkt und Tor-Isolations-Credentials konnten geloggt werden

**Schweregrad:** Mittel
**Vertrauen:** Hoch
**Klasse:** CWE-532 (Insertion of Sensitive Information into Log File)
**Umsetzungsstatus:** Behoben im geprüften Arbeitsbaum

#### Ausgangsbeobachtung

Die Paymaster-Prüfung verlangte für `PrivacyProfile::HIGH` korrekt `-capturemessages=0`, `-logips=0`, einen Onion-Proxy und randomisierte Proxy-Credentials (`CheckPaymasterPrivacyReadiness` in [`wallet/rpc/paymaster.cpp`](../src/wallet/rpc/paymaster.cpp)). [`CConnman::AddConnection`](../src/net.cpp) validierte dies ebenfalls. Der Privacy-Kontext wurde anschließend jedoch nicht bis zum generischen SOCKS5-Code weitergereicht.

Der SOCKS5-Unterbau protokolliert:

- Ziel beim Verbindungsaufbau;
- Ziel bei zwei Fehlerpfaden;
- Benutzername und Passwort der Proxy-Isolation bei `-debug=proxy`;
- Ziel bei erfolgreicher Verbindung unter `-debug=net`.

Die Credentials werden in [`ConnectThroughProxy`](../src/netbase.cpp) pro Verbindung erzeugt. Sie sind keine Wallet-Passphrase, aber ein Tor-Stream-Isolationstoken. Zusammen mit dem Onion-Ziel bilden sie einen eindeutigen lokalen Korrelationspunkt.

#### Auswirkung

Ein Leser von `debug.log`, Diagnosepaketen oder Log-Backups kann erkennen, welcher Onion-Provider zu welchem isolierten Stream gehörte. Dies widerspricht der im Paymaster-Netzwerkpfad ausdrücklich angestrebten Unterdrückung von Endpunkt-/Zeitkorrelation. Gelder oder private Schlüssel werden dadurch nicht unmittelbar gefährdet.

#### Empfehlung

1. Proxy-Benutzername und -Passwort grundsätzlich niemals ausgeben – unabhängig vom Paymaster.
2. Einen expliziten Privacy-Kontext, beispielsweise `ConnectionLogPolicy::PRIVATE`, von `AddConnection` bis `Socks5` weiterreichen.
3. Im privaten Kontext weder Zielhost noch Port noch Credential-Werte loggen; nur generische Ereignisse wie „private proxy connection failed“ zulassen.
4. Fehlertexte dürfen den privaten Zielhost nicht über indirekte `error()`-/Exception-Pfade wieder einführen.
5. Die High-Privacy-Bereitschaft nicht von Debug-Kategorien abhängig machen; die Vertraulichkeit muss auch bei `-debug=net,proxy` gelten.

#### Akzeptanztests

- SOCKS5-Erfolg, Timeout, Authentisierungsfehler und Zielablehnung mit bekanntem Onion-String und bekannten Credentials simulieren;
- Logs unter `-debug=net,proxy -logips=0` erfassen;
- sicherstellen, dass Onion-String, Port, Benutzername und Passwort nicht vorkommen;
- normale, nicht private Netzwerkdiagnostik darf weiterhin ausreichend Fehlerkontext liefern.

#### Umsetzung und Verifikation

Ein expliziter `ProxyLogPolicy` wird vom privaten Paymaster-Verbindungsaufbau bis `ConnectThroughProxy` und `Socks5` weitergereicht. SOCKS-Benutzername und -Passwort werden unabhängig vom Verbindungstyp nie ausgegeben. Im privaten Modus enthalten Erfolgs- und Fehlerlogs weder Zielhost noch Port; der normale Netzwerkpfad behält seinen nicht sensitiven Diagnosekontext.

Die neuen `netbase_tests` simulieren Erfolg, Zielablehnung und frühen Fehler mit bekannten Ziel- und Credential-Markern und prüfen deren Abwesenheit in privaten Logs. Der Test setzt zusätzlich einen bereits aktiven globalen SOCKS-Interrupt und stellt den vorherigen Zustand per RAII wieder her; damit ist er unabhängig von der Reihenfolge der Gesamtsuite. Die normalen `net_tests` blieben unverändert erfolgreich.

### PM-03 – Ausgangsbefund: BIP340-Identitätssignaturen verwendeten teilweise Null-Auxiliary-Randomness

**Schweregrad:** Niedrig
**Vertrauen:** Hoch
**Klasse:** kryptografische Härtung; keine behauptete BIP340-Signaturumgehung
**Umsetzungsstatus:** Behoben im geprüften Arbeitsbaum

#### Ausgangsbeobachtung

Mehrere neu erzeugte Paymaster-Identitätssignaturen übergaben `uint256{}` als Auxiliary Randomness, unter anderem Capacity Proofs in [`paymasterprovider.cpp`](../src/wallet/paymasterprovider.cpp) sowie Resultate, Alternative-Recovery-Responses und Quotes in [`wallet/rpc/paymaster.cpp`](../src/wallet/rpc/paymaster.cpp). Andere Signaturen, etwa Announcement und Restricted Descriptor, nutzten bereits `GetRandHash()`.

BIP340 erlaubt eine Null-Aux-Eingabe; die Basissicherheit der Signatur wird dadurch nicht aufgehoben. Frische Auxiliary Randomness verbessert jedoch die Robustheit gegen bestimmte Seitenkanal- und Fault-Injection-Szenarien. Die aktuelle Wahl scheint teilweise der bytegenauen Crash-/Retry-Idempotenz zu dienen.

#### Empfehlung

- Für jede **neue** Identitätssignatur `GetRandHash()` verwenden und das exakt signierte Artefakt vor extern sichtbaren Nebenwirkungen dauerhaft speichern.
- Retries müssen die persistierten Signaturbytes wiederverwenden, nicht neu signieren.
- Deterministische Signaturerzeugung nur an eng dokumentierten Crash-Fenstern beibehalten, an denen vor Persistenz zwingend eine identische Rekonstruktion benötigt wird.
- Capacity-Control-Proofs separat behandeln: deren absichtlich deterministischer Modus darf nicht ohne Idempotenzanalyse geändert werden.

#### Akzeptanztests

- zwei unabhängige Neuerzeugungen desselben Testinhalts dürfen unterschiedliche, gültige Signaturen erzeugen;
- Retry/Restart muss exakt dieselben persistierten Bytes liefern;
- nach Ablauf des Autorisierungsfensters darf keine neue Signatur entstehen, ein bereits persistierter exakter Retry bleibt zulässig.

#### Umsetzung und Verifikation

Alle produktiven Paymaster-Identitäts-, Ergebnis-, Quote-, Recovery- und Capacity-Control-Signaturen verwenden nun `GetRandHash()` für neu erzeugte BIP340-Signaturen. Capacity-, Quote-, Result- und Recovery-Artefakte werden vor Queueing oder Broadcast atomar gespeichert; Retry und Restart verwenden die exakt persistierten Signaturbytes. Bestehende Retry-Tests prüfen weiterhin die Bytegleichheit der wiederverwendeten Artefakte.

### PM-04 – Paymaster-Metadaten und signierte Artefakte liegen ohne Paymaster-spezifische Inhaltsverschlüsselung im Wallet-Datenbestand

**Schweregrad:** Niedrig
**Vertrauen:** Mittel
**Klasse:** Datenschutz-/At-rest-Risiko; CWE-312 ist abhängig vom endgültigen Schutzversprechen
**Umsetzungsstatus:** Schutzgrenze dokumentiert; bewusst akzeptiertes Restrisiko

#### Beobachtung

`ProviderAttempt` persistiert unter anderem Provider-Endpunkt, Intent/Capacity Request, Quote Request/Response, Unsigned Transaction, PSBT, user-signiertes PSBT, finale Transaktion, Manifeste und Equivocation-Kandidaten ([`reservation.h`](../src/paymaster/reservation.h)). Die Datensätze werden über die gewöhnliche Wallet-DB-Serialisierung geschrieben; eine Paymaster-spezifische Feld- oder Envelope-Verschlüsselung wurde nicht gefunden.

Positiv: Die geheime Sponsorship-Capability wird redigiert und nur als Hash-/Reservierungsbindung persistiert. Finale Sessions werden nach der Reorg-Sicherheitstiefe von 240 Blöcken zu Tombstones verdichtet. Ambige, nicht abgeschlossene oder beschädigte Sessions können die Metadaten jedoch länger behalten.

#### Auswirkung

Ein lokaler Leser der Wallet-Datei oder eines Backups kann Provider-Beziehungen, Zahlungsabläufe und signierte Artefakte korrelieren. Dies ist kein Private-Key-Leak und kann für Recovery notwendig sein, sollte aber als Datenschutzgrenze ausdrücklich dokumentiert werden – insbesondere bei `PrivacyProfile::HIGH`.

#### Empfehlung

- Das At-rest-Schutzmodell in der Betreiber-/Nutzerdokumentation klar benennen: Wallet-Verschlüsselung ist nicht automatisch gleichbedeutend mit Verschlüsselung aller Metadaten.
- Eine privacy-schonende Retention-Anzeige und sichere Bereinigung nur für kryptografisch eindeutige Terminalzustände vorsehen.
- Prüfen, ob Endpunkt und nicht mehr benötigte Zwischenartefakte nach sicherer Finalität entfernt oder durch minimale Referenzen ersetzt werden können.
- Feldverschlüsselung nur mit einem dokumentierten Schlüssel-/Unlock-/Recovery-Konzept einführen; keine Daten löschen, die für exakten Retry oder Recovery benötigt werden.

#### Umsetzung

`doc/digidollar-paymaster.md` beschreibt jetzt ausdrücklich die Grenzen der Wallet-Schlüsselverschlüsselung, die in Paymaster-Datensätzen und Vollbackups enthaltenen Metadaten, die 240-Block-Pruninggrenze, Tombstone-Inhalte und die längere Aufbewahrung nicht eindeutiger Zustände. Manuelle DB-Löschung und ein destruktiver Reset-RPC werden ausdrücklich verworfen. Feldverschlüsselung bleibt eine mögliche spätere Architekturentscheidung und darf erst mit einem belastbaren Schlüssel-, Unlock-, Backup- und Recovery-Modell erfolgen.

### PM-05 – Ausgangsbefund: CodeQL analysierte den Wallet-/RPC-Paymaster nicht

**Schweregrad:** Niedrig
**Vertrauen:** Hoch
**Klasse:** NIST SSDF PW.7/PW.8 – Assurance-Lücke
**Umsetzungsstatus:** Workflow-seitig behoben; erster CI-Service-Lauf steht noch aus

#### Ausgangsbeobachtung

Der vorhandene CodeQL-Workflow war grundsätzlich positiv und aktivierte `security-extended` sowie `security-and-quality`. Sein C/C++-Build konfigurierte jedoch mit `--disable-wallet --disable-tests`. Damit wurden die größten und sicherheitskritischsten Paymaster-Flächen – insbesondere `wallet/paymasterstore*.cpp`, `wallet/paymasterprovider.cpp` und `wallet/rpc/paymaster*.cpp` – nicht kompiliert und folglich nicht vollständig in die C++-Datenbank aufgenommen.

Außerdem enthält der sichtbare GitHub-CI-Workflow normale Unit-/Functional-Jobs, aber keinen expliziten Paymaster-Sanitizer-/Fuzz-Gate. Das ältere `ci/test`-Framework unterstützt ASan, UBSan, TSan, MSan, clang-tidy und Fuzzing, ist im geprüften GitHub-Workflow jedoch nicht als verpflichtender Paymaster-Job erkennbar. Ein dedizierter SBOM-/Dependency-Review-Workflow wurde ebenfalls nicht gefunden; externe Infrastruktur kann dies trotzdem abdecken.

#### Empfehlung

- einen wallet-aktivierten CodeQL-C/C++-Job oder einen zweiten manuellen Build hinzufügen;
- gezielte Paymaster-Unit- und Functional-Tests im Security-Job bauen;
- mindestens ASan+UBSan für Paymaster-Tests und die vier Fuzz-Ziele regelmäßig ausführen;
- TSan für Manager/Directory/Wallet-Callback-Grenzen periodisch einplanen;
- Dependency Review und maschinenlesbare SBOM für Release-Artefakte ergänzen;
- Ergebnisse als Merge-/Release-Gate behandeln, nicht nur als optionalen Bericht.

#### Umsetzung

Der CodeQL-C++-Job baut jetzt den wallet- und testfähigen `test_digibyte`-Pfad. Ein eigener Paymaster-Workflow führt die 16 gezielten Suites unter ASan+UBSan aus und startet zeitgesteuert die vier Paymaster-Fuzz-Ziele. Das neue wallet-abhängige Ziel `paymaster_persistence_records` speist beliebige Rohbytes in alle 35 statusbewussten Paymaster-Record-Decoder ein und prüft zusätzlich mutierte Session-, Template- und Unsigned-Txid-Indizes auf fail-closed Referenzauflösung ohne DB-Mutation. Zusätzliche Workflows führen Dependency Review bei Pull Requests aus und exportieren für Releases beziehungsweise manuelle Läufe eine SPDX-JSON-SBOM. Die Workflow-Dateien wurden lokal als YAML geparst; ihre erste Ausführung im GitHub-Service bleibt Release-Evidenz.

### PM-06 – Sehr große Orchestrierungs- und Persistenzdateien erschweren unabhängige Prüfung

**Schweregrad:** Informativ
**Vertrauen:** Hoch
**Umsetzungsstatus:** Behoben im geprüften Arbeitsbaum

Vor der Umsetzung umfasste `src/wallet/rpc/paymaster.cpp` ungefähr 14.400 Zeilen und `src/wallet/paymasterstore.cpp` ungefähr 11.300 Zeilen. Die starke lokale Testabdeckung reduzierte das Risiko, aber lange Funktionen und viele Zustandsdomänen in zwei Dateien erhöhten Review-Kosten und die Wahrscheinlichkeit, dass ein neuer Pfad etablierte Fail-closed-Helfer umgeht – PM-01 ist ein Beispiel dafür.

Die funktionalen Härtungsänderungen wurden zuerst als eigene Baseline committet. Anschließend wurden die RPC-Flächen in gemeinsame interne Helfer sowie Client, Discovery, Provider, Processing, Runtime und Wallet-Integration aufgeteilt. Die Persistenzimplementierung ist jetzt in gemeinsame Fail-closed-Helfer sowie Client, Provider, Finalisierung, Recovery, Reputation/Evidenz und Reconciliation/Pruning gegliedert. Alle Domänen sind eigenständige Übersetzungseinheiten; die öffentlichen Header und damit die Aufrufer-Schnittstellen blieben unverändert.

Die in die neuen Dateien verschobenen öffentlichen RPC- und `PaymasterStore`-Methoden wurden nach der Aufteilung in ursprünglicher Reihenfolge rekonstruiert und bytegleich mit der Härtungsbaseline verglichen. Der generierte MSVC-Build nahm sämtliche neuen Quellen aus `src/Makefile.am` auf, und die Wallet-Bibliothek kompilierte erfolgreich. Die bestehenden Persistenz-, Restart- und Fault-Injection-Suites bleiben die Verhaltensregression für diese Strukturgrenzen; ihr erneuter vollständiger Lauf nach der Aufteilung ist unter Abschnitt 7.1 als Operator-Abschlusslauf ausgewiesen.

Die Provider-Finalisierung besitzt zusätzlich eine automatisierte Restart-Matrix. Sie erzeugt jeweils einen abrupten DB-Snapshot nach der persistierten Signatur, nach dem atomaren Provider-Commit, nach der Wallet-Insertion und nach der lokalen Broadcast-Annahme. Jede Variante lädt eine neue Wallet-Instanz, führt die produktive Durable-Recovery mit synchronisiertem In-Memory-Txindex aus und verlangt denselben Commit, dasselbe signierte Resultat, exakt dieselben Witness-Bytes sowie genau eine ausgegebene Budgetreservierung. Ein zweiter Recovery-Lauf muss accounting-neutral bleiben.

### PM-07 – Functional-Test-Runner meldete einen Framework-Importfehler als Erfolg

**Schweregrad:** Niedrig
**Vertrauen:** Hoch
**Klassen:** CWE-754 (Improper Check for Unusual or Exceptional Conditions), NIST SSDF RV.1 – Assurance-Lücke
**Umsetzungsstatus:** Behoben im geprüften Arbeitsbaum

Beim ersten Windows-Functional-Lauf fehlte im ausgewählten Python-3.13-Interpreter das Modul `digibyte_scrypt`. Die Framework-Unit-Tests brachen deshalb vor jedem Paymaster-Test ab. `test/functional/test_runner.py` rief in diesem Fehlerpfad jedoch `sys.exit(False)` auf; Python bildet `False` auf Exit-Code 0 ab. Ein äußerer Build-, CI- oder Operatorprozess konnte einen vollständig ausgefallenen Testlauf daher als bestanden behandeln.

Der Fehlerpfad verwendet jetzt explizit Exit-Code 1. Der anschließende Lauf mit der korrekt eingerichteten Python-3.11-Umgebung lud alle Framework-Module und führte die fünf ausgewählten Paymaster-Functional-Tests tatsächlich aus. Zusätzlich berücksichtigt der RPC-Vertragstest den dokumentierten kurzen `PAYMASTER_PROVIDER_BUSY`-Zustand, in dem `stoppaymaster` neue Arbeit bereits verhindert, ein zuvor begonnener begrenzter Scheduler-Schritt seinen exklusiven Guard aber noch freigibt. Nur dieser konkrete transiente Fehler wird erneut versucht; alle anderen RPC-Fehler bleiben unmittelbare Testfehler.

## 6. Funktionale Auditmatrix

| Teilbereich | Ergebnis | Restarbeit |
|---|---|---|
| Protokoll-/Versionsverhandlung | Bestanden | Current-only beibehalten; keine impliziten Migrationen einführen. |
| Wire-Decoder/Canonical Encoding | Bestanden | Fuzz-Corpus kontinuierlich gegen Truncation, Trailing Bytes und Enum-Werte erweitern. |
| Directory/Announcement | Bestanden | Last-/Sybil-Annahmen unter realistischem Mainnet-Verkehr benchmarken. |
| Direct Transport/Ratelimits | Bestanden | Lasttest mit maximalen 1-MiB-Frames als zusätzliche Assurance. |
| Client-Auswahl und Intent | Bestanden | Property-Tests für Auswahlstabilität und maximale Attempts ergänzen. |
| Quote/Capacity | Bestanden | Auxiliary Randomness und exakte persistierte Retry-Semantik beibehalten. |
| PSBT-Rollentrennung | Bestanden | Mutationsfuzzing für alle PSBT-Maps fortführen. |
| Provider-Budget/Sponsorship | Bestanden | Typisierte Record-Reads bei neuen Ledger-Typen konsequent fortführen. |
| Commit/Broadcast/Idempotenz | Bestanden | Crash-Injection an jeder Persistenz-/Broadcast-Grenze automatisieren. |
| Self-/Alternative-Recovery | Bestanden | Historische Policy-Bindung weiter als Formatkompatibilität klar abgrenzen. |
| Session-/Attempt-Zustände | Bestanden | Vollständige Matrizen und `0xff`-Tests sind vorhanden. |
| Wallet-Unlock/Signaturzeitpunkt | Bestanden | Unlock unmittelbar vor jeder neuen Signatur beibehalten; exakte Retries dürfen keine Neusignatur erzwingen. |
| Pruning/Retention | Bestanden, Restrisiko dokumentiert | Vollständige CI-/Fault-Injection-Evidenz archivieren; At-rest-Modell bei Änderungen neu bewerten. |
| Provider-Pool/Finance | Bestanden | Gezielt geprüfte Fail-closed-Reads beibehalten. |
| Logging/High Privacy | Bestanden | Neue Proxy-Fehlerpfade jeweils durch Redaction-Tests absichern. |
| CI/SAST/Fuzz | Konfiguriert | Erste und nachfolgende Service-Läufe als Release-Evidenz archivieren. |

## 7. Test- und Werkzeugevidenz

### 7.1 Erfolgreich ausgeführt

Auf der Härtungsbaseline `8573b25bd9` wurden ausgeführt:

1. MSVC x64 Release-Build von `test_digibyte`: **Exit Code 0**.
2. 16 gezielte Paymaster-Test-Suites: **222/222 Testfälle**, **5.949/5.949 Assertions**, Exit Code 0.
3. `netbase_tests`: **16/16 Testfälle**, **307/307 Assertions**, Exit Code 0. Der einzelne SOCKS-Reihenfolge-Regressionsfall bestand zusätzlich mit **22/22 Assertions**.
4. `net_tests`: **22/22 Testfälle**, **145.485/145.485 Assertions**, Exit Code 0.
5. Vollständige MSVC-x64-Release-Testsuite nach dem SOCKS-Testisolationsfix: **3.646/3.646 Testfälle**, darunter 3 Testfälle mit Warnungen, **15.529.212/15.529.212 Assertions**, Exit Code 0.
6. Fünf Paymaster-Functional-Tests unter Windows mit Descriptor-Wallets: **5/5 Testfälle**, zusätzlich **17/17 Framework-Unit-Tests**, Exit Code 0; akkumulierte Testdauer 105 Sekunden.
7. Alle fünf GitHub-Workflow-Dateien wurden erfolgreich als YAML geparst.

Für die anschließende verhaltensneutrale Dateiaufteilung wurden zusätzlich ausgeführt:

8. Rekonstruktion aller verschobenen öffentlichen RPC- und `PaymasterStore`-Methoden mit bytegleichem Vergleich zur Härtungsbaseline: **keine Abweichung**.
9. Neugenerierung der MSVC-Projektdateien aus `src/Makefile.am`: **alle neuen Übersetzungseinheiten enthalten**.
10. MSVC-x64-Release-Build von `libdigibyte_wallet`: **Exit Code 0**.
11. Aktualisierter Paymaster-Security-Workflow einschließlich der neuen RPC-Pfadfilter: **YAML erfolgreich geparst**.
12. Vom Operator gemeldeter vollständiger MSVC-x64-Release-Lauf nach der Dateiaufteilung: **3.646/3.646 Testfälle**, Ausgabe `*** No errors detected`.
13. Aktualisierte `paymaster_wallet_security_tests`: **10/10 Testfälle**, **611/611 Assertions**, Exit Code 0; darin die vier Restart-Grenzen und ihr jeweils idempotenter zweiter Recovery-Lauf.
14. MSVC-Semantikprüfung des neuen Fuzz-Targets mit `/Zs /W3 /WX`: **Exit Code 0**. Der auf vier Ziele erweiterte Security-Workflow wurde erneut erfolgreich als YAML geparst.
15. Offizieller v9.26.5/current-Wallet-Kompatibilitätslauf unter x86_64 WSL: **17/17 Framework-Unit-Tests** und beide registrierten Varianten von `wallet_v9_26_5_compatibility.py` (**Legacy und Descriptor, 2/2**) bestanden, Exit Code 0; akkumulierte Kompatibilitätstestdauer 36 Sekunden.
16. Offizieller v9.26.5/current-Drei-Node-Lauf von `wallet_paymaster_provider.py --descriptors` unter x86_64 WSL: **17/17 Framework-Unit-Tests** und **1/1 Functional-Test** bestanden, Exit Code 0; Testdauer 27 Sekunden. Ein erster Lauf deckte auf, dass der Test nach einem Client-Restart nur die beiden aktuellen Nodes wieder verband und dadurch den alten Node vor `sync_all()` isolierte; nach Ergänzung der fehlenden Wiederverbindung bestand der vollständige Lauf.
17. Unabhängiger aktueller Drei-Node-Lauf von `wallet_paymaster_offer_selection.py --descriptors` unter x86_64 WSL: **17/17 Framework-Unit-Tests** und **1/1 Functional-Test** bestanden, Exit Code 0; Testdauer 27 Sekunden. Zwei eigenständige Paymaster-Daemons boten 50 beziehungsweise 200 Basispunkte an; der DGB-lose Client zeigte beide exakten Gesamtkosten sortiert an, wählte nur den günstigeren Provider und ließ Pool, Sicherheitsbudget und Finanz-Ledger des teureren Providers unverändert.
18. Gemeinsamer registrierter lokaler Windows-Lauf der drei erweiterten aktuellen Szenarien: **17/17 Framework-Unit-Tests** und **3/3 Functional-Tests bestanden**, Exit Code 0; 77 Sekunden akkumuliert.
19. Darin bestand `wallet_paymaster_offer_selection.py --descriptors` in 21 Sekunden. Zusätzlich zur bisherigen Auswahl wurden monotone Neupreisung, Wiederherstellung des günstigsten Angebots, Ablauf beider alten Announcements und Annahme ausschließlich eines frisch publizierten Ersatzes geprüft. `wallet_paymaster_failover.py --descriptors` bestand in 47 Sekunden: Ein Client hielt den einzigen günstigen operativen Slot, während ein zweiter mit unverändertem DD-Inputsatz sequenziell zum Standby-Provider wechselte; nach dem User-PSBT wurde weiterer Fallback abgelehnt, genau eine Zahlung verbucht und der unsignierte Konkurrent ohne Finanzwirkung freigegeben.
20. `wallet_paymaster_reorg.py --descriptors` bestand im selben registrierten Lauf in 9 Sekunden. Eine bestätigte Paymaster-Zahlung wurde durch eine längere transaktionsfreie Kette zurück in den Mempool reorganisiert; Client-Session, Finance-Event und Nachfolger-Liquidität rollten zurück und derselbe Txid wurde anschließend ohne doppeltes Finance-Event erneut bestätigt.
21. Derselbe registrierte aktuelle Drei-Test-Block bestand anschließend unter x86_64 WSL: **17/17 Framework-Unit-Tests** und **3/3 Functional-Tests**, Exit Code 0; `wallet_paymaster_offer_selection.py` 12 Sekunden, `wallet_paymaster_failover.py` 24 Sekunden und `wallet_paymaster_reorg.py` 3 Sekunden, insgesamt 39 Sekunden.
22. Der erste externe Lauf der neu registrierten offiziellen v9.26.5-Szenarien lieferte noch keinen Pass-Nachweis, aber zwei konkrete Fixture-Befunde: 1.000 DGB reichten für den v9.26.5-Tier-0-Mint nicht aus; außerdem führt v9.26.5 unbekannte `pmannounce`-Kommandos nicht in `getpeerinfo.bytesrecv_per_msg`. Die Fixtures wurden auf 10.000 DGB beziehungsweise den bereits etablierten Rohdaten-Parser von `-capturemessages=1` umgestellt. Der Bridge-Lauf hatte gewöhnliches DGB und DD zuvor bereits erfolgreich durch den alten Node übertragen und dort bestätigen lassen.
23. Der korrigierte offizielle v9.26.5/current-Bridge-Lauf unter x86_64 WSL bestand anschließend: **17/17 Framework-Unit-Tests** und `p2p_paymaster_v9_26_5_bridge.py --descriptors` **1/1**, Exit Code 0; Testdauer 38 Sekunden. Der alte Node leitete gewöhnliches DGB und DD weiter und bestätigte beides, empfing laut Rohdaten-Capture `pmannounce`, leitete es aber nicht weiter; ohne direkte aktuelle Verbindung blieb die Auswahl geschlossen, mit direkter Verbindung wurde das Angebot entdeckt. Der In-place-Test erstellte im selben Lauf bereits beide v9.26.5-Positionen und beide unbestätigten Transaktionen, traf dann aber auf das bei unverschlüsselten v9.26.5-Descriptor-Wallets fehlende optionale Feld `unlocked_until`; der Zustandsvergleich wurde daraufhin versionsneutral normalisiert.
24. Der abschließende offizielle In-place-Upgrade-Lauf unter x86_64 WSL bestand: **17/17 Framework-Unit-Tests** und `wallet_v9_26_5_inplace_upgrade.py --descriptors` **1/1**, Exit Code 0; Testdauer 7 Sekunden. v9.26.5 erstellte zwei verschlüsselte Descriptor-Wallets, zwei aktive DD-Positionen sowie isolierte unbestätigte DGB- und DD-Transaktionen. Der aktuelle Daemon lud auf demselben Datadir zunächst mit deaktiviertem Wallet unverändert Blocks, Chainstate, txindex und `mempool.dat`, anschließend beide Wallets und deren exakten Zustand; die gespeicherten Transaktionen wurden idempotent wieder angekündigt und bestätigt, eine alte Position wurde eingelöst und der Endzustand über einen weiteren Neustart erhalten.

Gezielte Suites:

`paymaster_client_tests`, `paymaster_directory_tests`, `paymaster_protocol_tests`, `paymaster_provider_tests`, `paymaster_psbt_tests`, `paymaster_recovery_tests`, `paymaster_reputation_tests`, `paymaster_reservation_tests`, `paymaster_sponsorship_tests`, `paymaster_txbuilder_tests`, `paymaster_types_tests`, `paymaster_wire_tests`, `paymaster_wallet_identity_tests`, `paymaster_wallet_psbt_tests`, `paymaster_wallet_security_tests`, `paymaster_wallet_store_tests`.

### 7.2 Nicht als aktuelle Release-Evidenz ausgeführt

- aktuelle ASan-/UBSan-/TSan-/MSan-Läufe;
- aktuelle Kampagnen der Fuzz-Ziele `paymaster_wire_envelopes`, `paymaster_stateful_security`, `paymaster_pool_lifecycle` und `paymaster_persistence_records`;
- ausgeführte CodeQL-Serviceanalyse;
- aktuelle CVE-/Dependency-/SBOM-Prüfung;
- erneuter vollständiger Lauf nach den zusätzlichen, produktionscode-neutralen Restart-/Fuzz-Teständerungen; die betroffene Security-Suite wurde stattdessen gezielt vollständig ausgeführt;
- erneuter `test_runner.py`-Lauf der erweiterten aktuellen Auswahl-, Failover- und Reorg-Szenarien in der WSL-/Release-Buildumgebung;
- Debug-Build auf Windows – ein früher beobachteter `0xC0000005`-Abbruch ist ohne aktuellen Stacktrace nicht als geklärt zu werten.

Cppcheck 2.14.0 war lokal vorhanden, konnte aber nicht belastbar ausgeführt werden: Das Binary wurde mit einem nicht vorhandenen `FILESDIR` auf Laufwerk `R:` gebaut und lädt deshalb selbst bei vorhandener lokaler `std.cfg` seine Standardbibliothek nicht. Es wurden daraus keine Befunde oder Entwarnungen abgeleitet.

### 7.3 Empfohlene Abschlussläufe

Windows, Repository-Root:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  build_msvc\digibyte.sln `
  -target:test_digibyte `
  -property:Configuration=Release `
  -property:Platform=x64 `
  -property:VcpkgRoot=C:\Users\johan\source\vcpkg\ `
  -maxCpuCount -verbosity:minimal

build_msvc\x64\Release\test_digibyte.exe `
  --log_level=error --report_level=short
```

Functional-Tests, in einer dafür konfigurierten Build-Umgebung:

```bash
python3 test/functional/test_runner.py \
  p2p_paymaster.py \
  wallet_paymaster_rpc.py \
  wallet_paymaster_lifecycle.py \
  wallet_paymaster_provider.py \
  wallet_paymaster_offer_selection.py \
  wallet_paymaster_failover.py \
  wallet_paymaster_reorg.py \
  wallet_paymaster_readiness.py \
  --jobs=1 --timeout-factor=2
```

Offizielle v9.26.5/current-Erweiterungen in derselben WSL-Buildumgebung:

```bash
V9_26_5_DIGIBYTED="$HOME/digibyte-releases/v9.26.5/bin/digibyted" \
python3 test/functional/test_runner.py --jobs=1 \
  wallet_v9_26_5_inplace_upgrade.py \
  p2p_paymaster_v9_26_5_bridge.py
```

Fuzzing mit aktuellem, sanitizer-aktiviertem Binary:

```bash
mkdir -p fuzz-corpus/paymaster_wire_envelopes \
         fuzz-corpus/paymaster_stateful_security \
         fuzz-corpus/paymaster_pool_lifecycle \
         fuzz-corpus/paymaster_persistence_records

for target in \
  paymaster_wire_envelopes \
  paymaster_stateful_security \
  paymaster_pool_lifecycle \
  paymaster_persistence_records
do
  FUZZ="$target" ./src/test/fuzz/fuzz \
    -max_total_time=600 "fuzz-corpus/$target" || exit $?
done
```

Erfolgskriterium: Exit Code 0, keine Sanitizer-Ausgabe und kein neues `crash-*`-/`leak-*`-Artefakt. Die großen Läufe sollten gemäß Repository-Anweisung durch den Operator beziehungsweise CI ausgeführt werden.

## 8. Priorisierte ToDos

### P0 – vor produktiver Freigabe

- [x] **PM-02:** Credentials aus SOCKS5-Logs entfernen und privaten Logging-Kontext bis `Socks5` propagieren.
- [x] **PM-02:** Log-Capture-Regressionstests für Erfolg, Zielablehnung und frühe Fehler hinzufügen.
- [x] **PM-01:** `DatabaseReadStatus` auf die sicherheitsrelevanten Paymaster-Datensätze ausdehnen.
- [x] **PM-01:** Pruning, Pool-, Finance-, Fee- und Recovery-Abgleich bei vorhandenen unlesbaren Records vor jeder Mutation abbrechen.
- [x] **PM-01:** Altformat-/Corruption-/No-Mutation-Tests für die kritischen abhängigen Recordpfade ergänzen.
- [x] vollständige MSVC-x64-Release-Testsuite ausführen und Ergebnis archivieren.
- [x] fünf Paymaster-Functional-Tests ausführen und Ergebnis archivieren.
- [ ] mindestens ASan+UBSan-Tests ausführen und Ergebnisse archivieren.

### P1 – Release-Härtung

- [x] **PM-03:** BIP340-Aux-Randomness für neue Identitätssignaturen einführen; exakte Retries aus persistierten Bytes bedienen.
- [x] wallet-aktivierten CodeQL-Job und gezielte Paymaster-Sanitizer-/Fuzz-Jobs einführen.
- [x] Crash-/Restart-Injection zwischen Signatur, DB-Commit, Wallet-Insertion und Broadcast automatisieren.
- [x] persistierte Record-Decoder und referenzielle Beziehungen mit einem eigenen Fuzz-/Property-Test abdecken.
- [x] Dependency Review und Release-SBOM ergänzen.
- [ ] CodeQL-, Dependency-Review-, SBOM-, Sanitizer- und Fuzz-Workflows erstmals im GitHub-Service ausführen und Ergebnisse archivieren.
- [ ] unabhängiges Review der finalen PM-01-/PM-02-Patches durchführen.

### P2 – Datenschutz und Wartbarkeit

- [x] **PM-04:** At-rest- und Backup-Datenschutzmodell dokumentieren.
- [ ] sichere Retention-/Cleanup-Strategie für Terminal- und dauerhaft ambige Sessions entwerfen.
- [x] `wallet/rpc/paymaster.cpp` und `wallet/paymasterstore.cpp` domänenweise aufteilen.
- [ ] privacy-schonende Betriebsmetriken verwenden: nur aggregierte Zähler/Fehlerklassen, keine IDs, Endpunkte, PSBTs oder Tx-Rohdaten.
- [ ] periodische TSan- und Langzeit-Fuzz-Läufe als Release-Evidenz etablieren.

## 9. Freigabeempfehlung

**Aktueller Stand:** Die bestätigten Befunde mittlerer Priorität sind im geprüften Arbeitsbaum geschlossen; der Stand ist für weitere Entwicklung und Testnet-/Regtest-Erprobung unter kontrollierten Bedingungen geeignet.
**Produktivfreigabe:** Die vollständige Release- und gezielte Paymaster-Functional-Verifikation war auf der Härtungsbaseline erfolgreich; auch der vollständige `test_digibyte`-Abschlusslauf nach der verhaltensneutralen Dateiaufteilung bestand. Vor einer Freigabe verbleiben aktuelle Sanitizer- und Fuzz-Nachweise, erstmalig erfolgreiche Security-Workflows sowie eine unabhängige Prüfung der finalen Änderungen.

Die bereits vorhandenen Schutzmechanismen sollten beim Beheben der Befunde nicht vereinfacht werden. Insbesondere sind exakte Artefaktbindung, historische Policy-Prüfung bereits signierter Autorisierungen, atomare Budget-/Commit-Persistenz, Wallet-Downgrade-Schutz und die Trennung von USER-/PROVIDER-Signaturrollen Sicherheitsinvarianten.
