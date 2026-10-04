# Capabilitati HomeKey-ESP32

Lista rezuma ce poate face firmware-ul descris in documentatia proiectului. Documentatia site-ului este pentru ramura `main` (development); firmware-urile lansate pot sa nu includa toate functiile de mai jos.

## Status fata de codul local

Status verificat in checkout-ul curent, nu dedus doar din documentatia publicata.

| Capabilitate | Status | Observatii / ce mai trebuie |
| --- | --- | --- |
| Autentificare HomeKey, NFC si pairing HomeKit | **IMPLEMENTAT** | NfcManager, PN532 si HomeSpan sunt in firmware. |
| Stari lock/unlock si control fizic | **PARTIAL** | Logica de stare si iesirea GPIO exista; releul/mecanismul lacatului si feedback-ul fizic sunt hardware extern. |
| Feedback GPIO si NeoPixel | **IMPLEMENTAT** | Actiunile sunt configurabile; necesita cablare si verificare pe placa finala. |
| Guest NFC tags | **IMPLEMENTAT, OPTIONAL** | Suport in firmware, dezactivat pana la activare; PN532, maximum 16 carduri/nod. |
| MQTT, comenzi household semnate si status | **IMPLEMENTAT** | Include interfata legacy si namespace household; comanda household foloseste HMAC. |
| Home Assistant direct prin HTTPS | **PARTIAL IN ACEST REPO** | Endpoint-urile firmware exista; componenta `homekey_household` este in repository separat si nu este verificata de acest checkout. |
| Automatizari Home Assistant | **EXTERN** | Firmware-ul publica stari/evenimente; regulile YAML ruleaza in Home Assistant. Nu exista un motor local de automatizari in `main/`. |
| Gospodarie, provisioning, health si audit | **IMPLEMENTAT** | Manager-ele si endpoint-urile sunt prezente in firmware. |
| Backup configuratie standard | **IMPLEMENTAT** | Backup criptat si semnat; fara cheile specifice cititorului/pairing-ului. |
| Backup optional cu credentials | **PARTIAL: pairing incomplet** | Reader store si unele date HAP sunt copiate, dar `CONTROLLERS` (lista controllerelor Apple imperecheate) nu este exportat/restaurat. Nu promite revenirea fara re-pairing. |
| OTA pe doua sloturi | **IMPLEMENTAT IN CONFIGURATIA GENERATA** | `sdkconfig` local selecteaza `with_ota.csv` si rollback; endpoint-ul OTA scrie slotul inactiv. Exista comentarii si fisiere de partitionare cu afirmatii vechi despre single-slot; trebuie reconciliate. |
| Web UI, configurare si loguri | **IMPLEMENTAT** | Configurarea, statusul si streaming-ul logurilor sunt in firmware. |
| Flash encryption, NVS encryption si Secure Boot | **IMPLEMENTATE, OPRITE** | Sunt dezactivate in configuratia locala; activarea lor este o decizie separata, ireversibila pe placa. |
| Hardware suportat de acest fork | **LIMITAT INTENTIONAT** | ESP32 clasic / ESP32-C3, Wi-Fi si PN532 SPI; nu Ethernet, PN7160/PN7161 sau ST25R3916. |

## Ce mai trebuie facut

1. **Backup pairing HomeKit:** exporta si restaureaza `CONTROLLERS` din namespace-ul `HAP` pentru optiunea `include_credentials`, apoi adauga un test de backup/restore cu si fara credentials. In prezent testul local de backup verifica in principal constructia criptografica, nu restaurarea datelor HomeSpan. Referinte: [BackupManager.cpp](main/BackupManager.cpp), [RestoreManager.cpp](main/RestoreManager.cpp), [HomeSpan HAP.cpp](components/HomeSpan/upstream/src/HAP.cpp), [test_backup_crypto.py](tests/test_backup_crypto.py).
2. **Curatare OTA:** aliniaza comentariile din [HomeKitLock.cpp](main/HomeKitLock.cpp), [WebServerManager.cpp](main/WebServerManager.cpp) si [no_ota.csv](no_ota.csv) cu buildul curent, care foloseste `with_ota.csv`. Nu este nevoie sa implementezi OTA de la zero.
3. **Verificare hardware:** testeaza PN532, releul/GPIO si comportamentul la pierderea retelei pe placa folosita efectiv; codul singur nu poate confirma montajul fizic.
4. **Automatizari fara Home Assistant (optional):** daca acesta este un obiectiv, trebuie adaugat un motor local. In prezent automatizarile documentate ruleaza in Home Assistant, nu in firmware.
5. **Hardening (optional):** criptarea si Secure Boot exista, dar sunt oprite implicit. Activarea lor nu este o remediere obisnuita si trebuie facuta doar urmand rollout-ul documentat, dupa backup si acceptarea consecintelor ireversibile.

## Acces si HomeKit

- Citeste autentificari Apple HomeKey de pe iPhone si Apple Watch printr-un cititor NFC PN532.
- Se imperecheaza cu Apple Home si apare ca un accesoriu de tip lacat HomeKit.
- Poate comanda starea logica lock/unlock si poate publica rezultatul catre HomeKit si sistemele conectate.
- Poate raporta HomeKey-ul care a fost folosit, inclusiv informatii despre issuer si endpoint, catre interfata locala si MQTT.
- Poate configura comportamentul la o autentificare HomeKey: de exemplu, sa forteze starea Locked sau Unlocked, indiferent de starea precedenta.
- Poate accelera autentificarea prin precomputarea unor date; optiunea foloseste mai multa memorie si procesare.

## Control hardware

- Poate actiona iesiri GPIO pentru relee sau alte circuite de comanda, cu nivel HIGH/LOW si impulsuri temporizate configurabile.
- Poate semnaliza o autentificare HomeKey reusita/nereusita sau o citire NFC generica prin GPIO si NeoPixel, cu culori si durate configurabile.
- Poate functiona in mod de comutator simplu ("dumb switch") si poate configura daca o atingere HomeKey actioneaza iesirea.

**Limita importanta:** proiectul nu implementeaza mecanismul fizic al unui lacat si nu masoara singur pozitia reala a usii. ESP32 furnizeaza stari logice si semnale GPIO; releul, motorul, senzorul de usa si logica de feedback trebuie furnizate/configurate separat.

## Carduri NFC pentru oaspeti

- Poate invata carduri NTAG213/215/216 si le poate accepta local ca o credentiala separata de Apple HomeKey.
- Poate seta o perioada de valabilitate, dezactiva accesul pentru oaspeti si revoca individual un card.
- Poate verifica aceste carduri fara Home Assistant, MQTT sau internet dupa ce configuratia a ajuns pe nod.
- Intr-o gospodarie cu mai multe noduri, tabelul de carduri poate fi distribuit prin MQTT, astfel incat fiecare nod sa verifice cardurile independent.
- Permite pana la 16 carduri per nod; invatarea cardurilor necesita un PN532.

Cardurile guest nu sunt credentiale Apple HomeKey si au securitate mai slaba: memoria lor poate fi clonata cu echipament NFC uzual. Pentru cardurile cu expirare, ceasul nodului trebuie sa fie sincronizat; dupa o pornire la rece fara retea, acestea sunt refuzate pana la sincronizarea SNTP.

## Integrare si automatizari

- Functioneaza independent pentru acces local; Home Assistant, MQTT si internetul nu sunt necesare pentru autentificarea HomeKey locala.
- Se integreaza cu Home Assistant fie prin MQTT, fie prin componenta separata `homekey_household`, direct peste HTTPS cu certificatul nodului fixat (fara broker MQTT).
- Expune starea lacatului, disponibilitatea nodului, sanatatea, securitatea, versiunea firmware, ultima autentificare si starea backupului. Componenta Home Assistant adauga si administrarea guest tags.
- Prin MQTT poate publica stari si evenimente NFC/HomeKey, permite comenzi lock/unlock si suporta valori personalizate pentru starile lacatului.
- Permite automatizari Home Assistant declansate de HomeKey, carduri NFC, issuer/endpoint sau schimbarile de stare ale lacatului. Automatizarile pot, de exemplu, sincroniza un lacat fizic separat sau controla lumini si alte dispozitive.
- In transportul Home Assistant direct, comunicarea foloseste HTTPS cu verificarea certificatului fixat; interogarile se fac periodic. Prin MQTT, actualizarea starii este de regula mai rapida.

Automatizarile din aceasta sectiune ruleaza in Home Assistant. Firmware-ul emite datele si comenzile necesare, dar nu contine un motor local de reguli; utilizarea MQTT/Home Assistant ramane optionala pentru deblocarea locala prin HomeKey.

## Administrarea unei gospodarii

Aceste functii sunt specifice acestui fork; proiectul upstream trateaza fiecare dispozitiv ca nod independent.

- Grupeaza mai multe dispozitive ESP32 in aceeasi gospodarie si atribuie fiecarui nod o identitate proprie.
- Poate inscrie un nod nou printr-un cod de asociere de unica folosinta, cu expirare.
- Ofera informatii agregate despre sanatate si un jurnal limitat de evenimente relevante pentru securitate.
- Poate exporta backupuri versionate, criptate si semnate, apoi restaura configuratia si apartenenta la gospodarie pe un nod de inlocuire.
- Backupul standard nu include identitatea privata a cititorului HomeKey sau cheile endpoint-urilor. Optiunea `include_credentials` le include, impreuna cu anumite date HAP, dar implementarea locala nu copiaza tabelul `CONTROLLERS` cu pairing-urile Apple. Prin urmare, restaurarea unui backup cu credentials nu garanteaza inca revenirea fara re-pairing; vezi lista de lucru de mai sus.

## Configurare si actualizari

- Ofera interfata Web pentru configurarea Wi-Fi, HomeKit, MQTT, NFC, GPIO, autentificarii Web si HTTPS.
- Afiseaza versiunea, uptime-ul, memoria libera, semnalul Wi-Fi, starea NFC/MQTT si loguri live cu niveluri filtrabile.
- Permite repornirea dispozitivului, pornirea punctului de acces de configurare si resetarea imperecherii HomeKit sau a datelor Wi-Fi.
- Suporta actualizari de firmware prin LAN/OTA, folosind HTTPS si autentificare, sau prin cablu serial. Actualizarea OTA foloseste doua sloturi si poate reveni la firmware-ul anterior daca noul firmware nu confirma pornirea.
- Scriptul `scripts/ota_update.py` poate detecta dispozitivul, alege calea potrivita si actualiza mai multe dispozitive intr-o sesiune.

## Hardware si limite ale fork-ului

- Tintele documentate si construite sunt ESP32 clasic si ESP32-C3; reteaua este Wi-Fi.
- Singurul cititor NFC suportat de acest fork este PN532 prin SPI. Ethernet, PN7160/PN7161 si ST25R3916 sunt eliminate fata de upstream.
- Firmware-ul dispozitivului nu descarca singur actualizari de pe GitHub. Un update prin retea trebuie initiat de un utilizator autentificat; update-ul serial necesita acces fizic la dispozitiv.
- Criptarea flash, criptarea NVS si Secure Boot V1 sunt implementate, dar dezactivate implicit. Activarea lor este o operatiune ireversibila pe dispozitiv si necesita urmarea procedurii de securitate din documentatie.

## Documentatie consultata

- [Pagina principala](https://csepregiartur.github.io/HomeKey-ESP32/)
- [Setup](https://csepregiartur.github.io/HomeKey-ESP32/setup/)
- [Configurare](https://csepregiartur.github.io/HomeKey-ESP32/configuration/)
- [MQTT](https://csepregiartur.github.io/HomeKey-ESP32/mqtt/)
- [Gospodarie si noduri](https://csepregiartur.github.io/HomeKey-ESP32/household/)
- [Integrare Home Assistant](https://csepregiartur.github.io/HomeKey-ESP32/home-assistant/)
- [Guest NFC Tags](https://csepregiartur.github.io/HomeKey-ESP32/guest-tags/)
- [Automatizari Home Assistant](https://csepregiartur.github.io/HomeKey-ESP32/automations/)
- [Actualizari firmware](https://csepregiartur.github.io/HomeKey-ESP32/updates/)
- [Securitate](https://csepregiartur.github.io/HomeKey-ESP32/security/)
- [Fork comparat cu upstream](https://csepregiartur.github.io/HomeKey-ESP32/fork-vs-upstream/)