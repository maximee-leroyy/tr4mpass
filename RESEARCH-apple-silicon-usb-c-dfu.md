# checkm8 sur Mac Apple Silicon (M-series) en USB-C → Lightning direct

**Objectif de cette note** : documenter tout ce qui a été appris en tentant de faire
passer checkm8 sur un Mac Apple Silicon avec **un câble Lightning branché
directement sur un port USB-C**, donc

- **sans hub USB** (et sans adaptateur USB-A),
- **sans débrancher / rebrancher** le câble à un moment précis,
- hôte macOS / XHCI Apple Silicon, backend IOKit maison (`src/dfu/iokit_usb.c`).

Cible de test : **iPhone 6s (A9), CPID `0x8000`**, `SRTG:[iBoot-2234.0.0.3.3]`,
DFU `05ac:1227`, `iSerialNumber` = index 4.

> **Note (E20)** : `SRTG` = *SecureROM Tag* — c'est la version du
> **BootROM** de l'A9, **pas** celle d'iOS. `iBoot-2234.0.0.3.3` est
> donc identique sur TOUS les iPhone 6s quelle que soit la version iOS
> installée. Vérifié empiriquement : les iBSS des IPSW 9.0 (2817.1.93),
> 9.0.2 (2817.1.94), 9.1 (2817.10.34) et 9.3.5 (2817.60.2) n'ont jamais
> cette valeur. ⇒ **le serial DFU ne permet pas de déduire la version
> iOS** ; il faut la lire sur le device (Settings > General > About) ou
> via `ideviceinfo` en mode normal.

Ce document rassemble : pourquoi gaster/palera1n ne servent pas de référence ici,
les contraintes XHCI constatées sur le terrain, les preuves de log, les stratégies
de récupération qui marchent, les pièges (dont le « faux succès »), et les pistes
restantes.

> **STATUT (E19, 2026-11-01) : OBJECTIF ATTEINT ✅** — `PWND:[checkm8]`
> obtenu sur cette configuration exacte (câble direct, sans hub, sans
> replug) avec le port tr4mpass. Recette gagnante en §11/E19 :
> IOKit de bout en bout + cycle stage 1 gaster + reset inter-stage à
> chaque frontière + reset final (trigger d'exécution) + overwrite
> 0x02/0x03+48 + résultats post-overwrite ignorés.

---

## 1. Pourquoi gaster ne peut pas être copié verbatim

palera1n (qui embarque gaster) documente explicitement que cette configuration
n'est **pas** supportée de façon fiable :

> « USB-C port on Apple Silicon Macs may require manual unplugging and replugging
> of the lightning cable **after checkm8 exploit**. This problem may be solved by
> connecting via USB hub. »
>
> « USB-A cables are recommended to use, USB-C to may have issues with palera1n
> and getting into DFU mode. »

Autrement dit : le hub / le replug sont des **contournements d'infrastructure**
utilisés par la communauté, pas une garantie que le flow gaster fonctionne tel
quel. Le `checkm8_stage_patch()` de gaster suppose un EP0 qui se comporte comme
sur un contrôleur Intel ; sur l'XHCI d'un M-series en USB-C direct, plusieurs de
ses hypothèses tombent (voir §3).

**Conséquence** : tout le code macOS de `checkm8_stage_patch()` dans `tr4mpass`
est une **tentative de contournement maison**, pas un portage de gaster. Les
commentaires du code doivent le dire clairement (cf. §8, état du dépôt).

Éléments de gaster conservés comme *référence de protocole* (et non de transport) :

| Élément | gaster | Notes |
|---|---|---|
| Ordre | overwrite → STALL → payload → suffix 16 o → zero-len DNLOAD → 3× GETSTATUS → ResetDevice + ReEnumerate | ordre conservé |
| Taille payload A9 | 528 o (176 main + 104 tail + 248 handler) | identique chez nous |
| Overwrite | 48 o, `callback = insecure_memory_base` | identique |
| `usb_timeout` | **5 ms** (env `USB_TIMEOUT`, défaut 5) | voir §3.4 |
| Payload | **un seul** DNLOAD de 528 o, **résultat ignoré** | voir §3.2 |
| CLR_STATUS du spray | `wLength = 3*64+1 = 193` **avec buffer de zéros** | voir §3.3 |
| Fin de stage | `ResetDevice` + `USBDeviceReEnumerate` | identique |

---

## 2. Architecture actuelle (rappel)

```
stage 1  reset DFU        -> libusb
stage 2  setup (UAF)      -> libusb, cancel asynchrone obligatoire
        ── bascule ──> fermeture libusb + activation IOKit (DeviceRequestTO)
stage 3  spray            -> IOKit
stage 4  overwrite + payload -> IOKit
```

La bascule libusb → IOKit après le stage 2 est **obligatoire** : l'annulation
asynchrone du stage 2 déclenche `AbortPipeZero` dans le backend darwin de libusb,
ce qui laisse la machine à états async de libusb inutilisable pour la suite.
`DeviceRequestTO` (IOKit) est synchrone et contourne ce problème.

Détails utiles :

- `EP0_MAX_PACKET_SZ = 0x40` (64), `DFU_MAX_TRANSFER_SZ = 0x800` (2048).
- A9 : `insecure_memory_base = 0x180380000`, `config_overwrite_pad = 0x500`.
- Le marker attendu dans le serial est `PWND:`.

---

## 3. Contraintes XHCI Apple Silicon constatées empiriquement

### 3.1 Un STALL n'auto-efface pas le halt d'EP0

Sur Intel/libusb, après un `SET_FEATURE(ENDPOINT_HALT)` qui STALLe, la requête
suivante repart normalement. Sur l'XHCI Apple Silicon, non : l'EP0 reste halté
côté contrôleur et toute requête suivante est refusée tant qu'un
`USBDeviceAbortPipeZero` n'a pas été émis.

### 3.2 Les DATA stages multi-paquets ne « complètent » pas

Un `DFU_DNLOAD` de 528 octets = 9 paquets de 64. Via `DeviceRequestTO`, la
requête rend `kIOReturnTimeout` (`ret=-7`) alors que la phase DATA a bien été
DMAée. C'est exactement pour cela que gaster **ignore** le résultat de son
DNLOAD de payload : il n'a jamais eu besoin de le vérifier, et sur M-series ce
retour est systématiquement trompeur.

Découper en paquets de 64 octets a été essayé : cela passe (chaque DNLOAD tient
dans un seul paquet), mais **ne suffit pas** à faire apparaître le PWND (voir
§7). Le découpage est donc une option, pas une condition nécessaire.

### 3.3 Un seul `AbortPipeZero` après le STALL — jamais deux

Preuve A/B sur device :

| | Run A (1 `AbortPipeZero`) | Run B (2 `AbortPipeZero`) |
|---|---|---|
| Après STALL + abort | suffix `16` OK, zero-len `0` OK, GETSTATUS `0x06→0x07→0x08` | 15 transferts consécutifs `-7`, définitif |
| Conclusion | le contrôleur « avale » 1–2 transferts puis se ré-arme | le 2e abort re-empoisonne le ring |

Corollaire : le ring se ré-arme **au bout de ~10–11 s**, pas au bout de N
transferts. Dans le Run A, deux transferts à timeout de 5 s ont été « mangés »
avant que le troisième (le suffix) passe. Un sondage court répété (500 ms) ne
mesure pas la même chose : chaque annulation de transfert est un mini-abort, et
c'est précisément ce qui a reproduit la pathologie du Run B.

### 3.4 Les timeouts longs transforment un échec récupérable en perte de service

Chaîne observée :

```
0xe00002ed  kIOReturnNotResponding   (fin de spray, ou requête ambiguë laissée pendante)
   └─> le service IOService est démonté
       └─> transfert suivant : 0xe00002c0 kIOReturnNoDevice
           └─> ré-ouverture : « DFU device not found » si on ne patiente pas
```

Une requête ambiguë (leak `GET_DESCRIPTOR`, `CLR_STATUS` à phase DATA non
acquittée) laissée pendante **plusieurs secondes** escalade vers
`NotResponding`. Avec un budget de quelques millisecondes, le contrôleur annule
proprement et le handle survit. C'est la raison du timeout « gaster »
`GASTER_USB_TIMEOUT_MS = 5` introduit dans `include/dfu/checkm8_internal.h`.

### 3.5 Le service peut disparaître puis revenir — c'est normal et récupérable

La ré-énumération du device prend typiquement **250 ms à quelques secondes**.
Un `IOServiceGetMatchingService` en un seul essai dans cette fenêtre échoue
franchement (`DFU device not found`) alors que le device revient juste après.

Correctif appliqué : `iokit_usb_activate()` **sondage de l'IORegistry**
(24 × 250 ms ≈ 6 s) au lieu d'un match unique, plus auto-réparation dans
`iokit_usb_ctrl_transfer()` / `iokit_usb_abort_pipe_zero()` : si le handle est
inactif, on tente de le ré-ouvrir avant de rendre `-4`.

Important : **l'état heap du stage 2/3 survit à la ré-énumération** côté device.
Preuve : après `NoDevice` + ré-acquisition, le `overwrite` STALLe encore
(`STALL confirmed`) — le `io_buffer` libéré est toujours là.

### 3.6 Le serial ne se lit pas par le chemin LANGID

`libusb_get_string_descriptor_ascii()` sonde les LANGID en premier ; sur Apple
Silicon cette sonde part en timeout et **empoisonne le pipe** (les GET_STATUS du
stage 1 échouent ensuite). Le code lit donc le descripteur en UTF-16LE
directement (`read_string_descriptor_utf16_ascii`, `src/device/usb_dfu.c`).

---

## 4. Table des codes IOReturn utiles

| Code | Nom | Interprétation pratique |
|---|---|---|
| `0xe00002c0` | `kIOReturnNoDevice` | service mort → ré-acquérir (souvent ré-énumération en cours) |
| `0xe00002d8` | `kIOReturnNotReady` | service pas prêt → patienter / ré-activer |
| `0xe00002ed` | `kIOReturnNotResponding` | requête pendante trop longtemps → service démoli |
| `0xe00002c5` | `kIOReturnExclusiveAccess` | libusb tient encore le device |
| `0xe000404f` | `kIOUSBPipeStalled` | **c'est le STALL attendu** de l'overwrite |
| `0xe000400e` | `kIOUSBNotSent1Err` | DATA stage non envoyée (multi-paquet) |

États DFU : `0x02` dfuIDLE, `0x05` dfuDNLOAD-IDLE, `0x06` dfuMANIFEST-SYNC,
`0x07` dfuMANIFEST, `0x08` dfuMANIFEST-WAIT-RESET.

---

## 5. Séquence de stage 4 (version contournement, état de référence)

```
prime          AbortPipeZero (+ GETSTATE de contrôle)
overwrite      SET_FEATURE(0x02,0x03,wIdx=0x80) 48 o  -> require STALL
halt           UN SEUL AbortPipeZero
récupération   sondage GETSTATE (courtes puis longues) jusqu'à réponse
payload        DNLOAD 528 o (découpé 64 o, ou en un bloc)
suffix         16 octets de zéros
manifest       DNLOAD 0 octet -> MANIFEST-SYNC
walk           3× GETSTATUS (0x06 -> 0x07 -> 0x08) : le callback UAF tire ici
reset          ResetDevice + USBDeviceReEnumerate
```

Points durs :

- **Ne jamais** envoyer `DFU_ABORT`/`CLR_STATUS` entre le stage 2 et le stage 3 :
  cela corrompt le ring EP0, le device ré-énumère, `kIOReturnNoDevice` en boucle.
  Le `CLR_STATUS` (`skip_clrstatus = 0`) émis par le spray fait la transition.
- Le STALL de l'overwrite est **la preuve** que l'état heap a survécu : s'il
  n'arrive pas, l'UAF est perdu → échouer vite plutôt que d'envoyer un payload
  dans un buffer DFU vierge.

---

## 6. Pièges de diagnostic

### 6.1 Le faux succès

Si les chunks du payload sont tous mangés (`-7`) mais que la séquence
suffix / zero-len / GETSTATUS est émise quand même, le BootROM traverse
`0x06 → 0x07 → 0x08` sur un **buffer vide**. Tous les stages « complètent », aucun
PWND n'apparaît. Garde-fou : compteur de timeouts consécutifs, abandon explicite
au lieu de déclarer « payload sent ».

### 6.2 `ret=-99` (`LIBUSB_ERROR_OTHER`) sur l'overwrite

C'est `kIOUSBPipeStalled` (`0xe000404f`) remonté par le backend **après** une
ré-acquisition de service, donc mal classé en erreur générique. Le log du dernier
run le montre :

```
overwrite AArch64 (callback=0x180380000, 48 bytes)
iokit_usb: kIOReturnNoDevice -- stale IOService, re-activating handle and retrying
iokit_usb: DeviceRequestTO failed: IOReturn=0xe000404f wLenDone=0
[ERROR] overwrite did not STALL (ret=-99, expected -9)
```

→ corriger le retry interne de `iokit_usb_ctrl_transfer()` : il doit **re-mapper**
le code IOReturn du second essai (STALL, timeout, …) au lieu de tomber dans le
`LIBUSB_ERROR_OTHER` final. **[Fait — voir §8]**

### 6.3 Device « détecté mais mort » après un bail-out

Un abandon en stage 4 laisse EP0 halté côté BootROM. Le device énumère toujours
(donc la détection réussit) mais **tout** GET_STATUS du stage 1 suivant part en
timeout. Deux correctifs en place :

- chemin d'erreur de `dfu_deliver()` : `iokit_usb_recover_bus()` (ResetDevice +
  ReEnumerate) au lieu d'un simple `deactivate()`;
- pré-vol dans `checkm8_run()` : un GET_STATUS ; en cas d'échec, log
  `device unresponsive (stale EP0 from a previous run)` puis récupération bus.
  Validé sur device : le run suivant repart en stage 1 normal.

---

## 7. Pistes ouvertes (mise à jour après la série E2→E6)

1. **Pourquoi le PWND n'apparaît pas alors que le walk MANIFEST passe.**
   ~ Élucidé (§11, Run A décrypté) : le payload DNLOAD portait un budget de
   5 s alors que le ring se ré-arme à ~10 s — ses octets n'ont jamais atteint
   le buffer DFU ; MANIFEST marchait sur un buffer vide.
2. **Fenêtre de récupération EP0 après le STALL.** Mesurée : ~10 s (Run A :
   transfert soumis à t+5 s complété à t+10 s). Un transfert dont le budget
   enjambe le moment de récupération se complète normalement.
3. **Faut-il un `AbortPipeZero` après le STALL de l'overwrite ?** Tranché :
   **NON.** Preuve A/B accumulée sur 6 runs (§11) — c'est le poison.
4. ~~Budget de timeout du payload.~~ Corrigé : `USB_EP0_RECOVERY_TIMEOUT_MS`
   (30 s) sur le DNLOAD de payload (E6, en cours de validation device).
5. **Le `0xe00002ed` de fin de spray.** Résolu côté boucle : le transport
   async+abort (abort **pendant** que la requête est pending) rend la boucle
   stall/leak/no_leak parfaitement propre (E5 : 100 itérations sans un seul
   wedge). Le CLR_STATUS final wedge toujours le device une fois (0xe00002ed,
   CLR `-99`) mais est **sans conséquence** : le service revient en 250 ms,
   l'état heap survit, et le stage 4 se poursuit normalement.
6. **Alternative transport (hub USB-A).** Toujours pertinente comme référence
   « qui marche » pour isoler ce qui relève du contrôleur vs du device.

---

## 8. État du dépôt (à garder à jour)

Fichiers impliqués :

| Fichier | Rôle |
|---|---|
| `src/dfu/iokit_usb.c` | backend IOKit : activate (sondage IORegistry), transferts, abort, serial, reset, `recover_bus` |
| `src/dfu/checkm8.c` | orchestration, bascule libusb→IOKit, pré-vol, `checkm8_verify`, retry |
| `src/dfu/checkm8_stages.c` | helpers de transfert, stage 1, stage 2 |
| `src/dfu/checkm8_alloc.c` | stage 3 (spray) + `CLR_STATUS` |
| `src/dfu/checkm8_patch.c` | stage 4 |
| `include/dfu/checkm8_internal.h` | constantes (dont `GASTER_USB_TIMEOUT_MS`) |
| `src/device/usb_dfu.c` | lecture du serial (UTF-16LE, pas de sonde LANGID) |

Statut au moment de la rédaction :

- **validés sur device** : stage 1, stage 2 (macOS timeout-comme-STALL), stage 3
  (spray `stall+leak+no_leak` via transport async+abort gaster — réussi du
  premier coup une fois le transport corrigé, voir §11/E5), pré-vol/
  récupération bus, ré-acquisition de service, lecture du serial.
- **décision** : la copie fidèle du flow gaster (payload en un seul DNLOAD,
  résultat ignoré, timeouts 5 ms, pas d'abort après STALL) a été d'abord
  **revert** (option A) puis partiellement réadoptée : le transport async+abort
  du spray et le CLR 193-zéros ignoré sont gaster, mais le stage 4 garde les
  adaptations M-series (motif : §11, l'abort post-STALL est un poison et le
  payload doit porter un timeout long).
- **correctifs appliqués** : re-mapping IOReturn du second essai (§6.2) ;
  `iokit_usb_ctrl_transfer_async_abort()` (transport async gaster) ; sémantique
  gaster sur le résultat async (`sz == 0` suffit, pas le statut).
- **non validé** : stage 4 en version E6 (payload DNLOAD à budget 30 s
  enjambant la fenêtre de récupération ~10 s, sans abort post-STALL).
  Historique complet des essais : §11.

Commits de référence pour l'historique des décisions :
`2242f19`, `8cf45a9`, `4d08282`.

---

## 9. Commandes utiles

```sh
# build + tests
make && make test

# run
./tr4mpass            # détection + path_a (checkm8) automatique
./tr4mpass -v         # logs debug

# vérifier le marqueur sur le device en DFU
system_profiler SPUSBDataType | grep -A6 "1227"
ioreg -p IOUSB -l -w 0 | grep -i "USB Serial Number"
```

Critères de succès d'un run :

```
overwrite STALL confirmed (UAF write landed)
GETSTATUS[0] state=0x06 / [1] 0x07 / [2] 0x08
serial contient PWND:  ->  checkm8_verify: device processed successfully
```

---

## 10. Références

- gaster (palera1n) : `checkm8_stage_patch()` (`gaster.c`), `usb_timeout` (5 ms),
  `send_usb_control_request_no_data()` (buffer de zéros de `w_len` octets),
  `reset_usb_handle()` = `ResetDevice` + `USBDeviceReEnumerate`.
- README palera1n : mise en garde USB-C / Apple Silicon (replug après exploit, hub).
- Historique du dépôt : `2242f19` (pourquoi le backend IOKit existe),
  `8cf45a9` (DATA stages multi-paquets rejetées), `4d08282`.

---

## 11. Journal des expériences (chronologique)

Chaque entrée : hypothèse → test → résultat → conclusion. Les logs sont
conserver dans `/tmp/tr4mpass-*.log` du poste de test.

### E0 — État initial (runs utilisateurs, avant ce journal)

**Observations** : stages 1-3 OK, stage 4 « complète » toujours, jamais de
PWND. Deux variantes testées : payload-first (callback jamais armé) puis
gaster-order (overwrite d'abord) avec AbortPipeZero + canary GETSTATE
après le STALL.

**Constat clé** : `payload offset=0 chunk=528 ret=-7` (DNLOAD 528 o en
timeout) mais suffix/zero-len/GETSTATUS OK → le device traversait MANIFEST
**sans payload** → faux succès structurel (§6.1).

### E1 — « gaster-faithful » (copie stricte du flow gaster)

**Hypothèse** : si gaster marche sur M-series via hub, sa séquence exacte
(5 ms partout, payload mono-DNLOAD ignoré, zéro abort) doit marcher ici.

**Test** : stage 4 réécrit en fidélité gaster stricte (§10), timeouts 5 ms,
CLR 193 zéros, payload mono-DNLOAD résultat ignoré.

**Résultat** : échec immédiat et reproductible — après le spray, l'overwrite
rend `0xe000404f` remappé `ret=-99` après ré-acquisition de service
(`overwrite did not STALL (ret=-99)`).

**Conclusions** :
- gaster n'est **pas** une référence transport pour USB-C direct Lightning
  (personne ne supporte cette config — c'est nous qui devons la faire marcher) ;
- MAIS l'échec était en partie un **faux négatif** : `0xe000404f` EST le STALL
  attendu, mal remappé (correctif §6.2 appliqué ensuite) ;
- décision utilisateur : **revert** (option A) vers la version contournement.

### E2 — Canary sans annulations rapides + CLR conditionnel

**Hypothèses** : (a) les sondes courtes annulées tôt sont des mini-aborts qui
re-empoisonnent le ring (Run B) → des sondes pleines de 5 s devraient
récupérer comme Run A ; (b) le CLR_STATUS fait ré-énumérer le device et
détruit l'état UAF → ne l'envoyer que si le device n'est pas en dfuIDLE.

**Test** : sonde GETSTATE 5×5 s ; sonde GETSTATE avant CLR, skip si 0x02.

**Résultat** (`/tmp/tr4mpass-e2.log`) : canary 5/5 `-7` (~25 s) → l'hypothèse
(a) est **falsifiée**. Le GETSTATE de sondage échoue déjà (`ret=-7`) → le CLR
part quand même → `0xe00002ed` → ré-énumération → stage 4 sur device ré-énuméré.

**Conclusions** :
- après le STALL de l'overwrite, **aucun transfert IN ne passe** (500 ms ni
  5 s) alors qu'avant l'overwrite tout passait ;
- `pre-overwrite GETSTATE state=0x02` (dfuIDLE) après le spray+CLR.

### E3 — Payload OUT-first (pas de canary) + aborts après leak/no_leak

**Hypothèses** : (a) après un STALL en plein DATA OUT, le ring ne se ré-arme
que sur un transfert OUT → le payload chunk doit passer là où les IN mouraient ;
(b) aborter leak/no_leak juste après leur timeout « snaps » le device hors de
la requête pendante (comme l'async de gaster) et empêche le wedge.

**Test** : suppression du canary, chunks 64 o directs ; `AbortPipeZero` après
chaque leak/no_leak synchrone.

**Résultat** (`/tmp/tr4mpass-e3.log`) : chunk OUT également mangé (3× `-7`,
garde coupe à 15 s) → (a) **falsifiée** : la direction n'est pas le facteur.
Les aborts post-timeout ne changent rien au wedge final (`0xe00002ed` au CLR).

**Conclusion** : le facteur n'est ni la direction ni le budget ; c'est
l'**abort lui-même** (tous les runs avec abort post-STALL meurent ; Run A
sans abort est le seul qui a récupéré) — mais l'abort après leak/no_leak
synchrone est de toute façon **trop tard** : côté device la requête reste
pendante après le timeout hôte.

### E5 — Transport async+abort réel pour le spray (gaster) + canary 5×5 s

**Hypothèse** : l'async de gaster (DeviceRequestAsyncTO + abort à 1 ms
**pendant** que la requête est pending) est la bonne primitive ; nos
aborts post-timeout n'équivalent pas à l'abort en flight.

**Test** : `iokit_usb_ctrl_transfer_async_abort()` (DeviceRequestAsyncTO,
sleep 1 ms, USBDeviceAbortPipeZero, RunLoop pour le callback), routé dans
leak/no_leak.

**Premier run** (`/tmp/tr4mpass-e5.log`) : la boucle du spray tourne 100
itérations **sans aucun wedge** (le service survit enfin à la boucle !) mais
`LEAK OK` jamais atteint : mon mapping retournait `-7` sur
`kIOReturnAborted+wLenDone=0` alors que gaster ne regarde que `sz == 0`.

**Correctif** : sémantique gaster sur le retour async (0 octets transférés =
succès, quel que soit le statut) + CLR final gaster simple (193 zéros, 5 ms,
résultat ignoré, sans sonde ni pré-abort).

**Deuxième run** (`/tmp/tr4mpass-e5b.log`) : spray **OK du premier coup**
(`stall+leak+no_leak OK (attempt 1)`), CLR wedge le device une fois
(`0xe00002ed`, `-99` ignoré), service revient en 250 ms, overwrite **STALL
confirmé** … puis abort post-STALL (encore présent dans cette version) →
ring mort, tout `-7` jusqu'au ResetDevice final. Pas de PWND.

**Conclusions majeures** :
1. **Le transport async+abort est LA bonne primitive** : la boucle spray est
   propre, reproductible, du premier coup.
2. **L'état heap du UAF survit à la ré-énumération** : après le wedge du CLR +
   ré-énumération (250 ms) + ré-acquisition de service, l'overwrite STALLE
   encore. (Cohérent avec gaster : `reset_usb_handle()` fait ResetDevice +
   ReEnumerate **entre chaque stage**.) L'hypothèse E2 « ré-énumération =
   UAF détruit » était fausse — le STALL de l'overwrite n'est pas pour autant
   une preuve de UAF vivant (un DFU frais STALLE aussi SET_FEATURE), mais la
   survie du heap est maintenant démontrée positivement.
3. Le `0xe00002ed`/CLR final est **cosmétique** : ré-énumération gérée
   transparentement, aucun impact sur la suite.
4. Reste UN seul écart mortel avec Run A : **l'AbortPipeZero post-STALL**.

### E6 — Suppression de l'abort post-STALL + payload à budget long

**Hypothèse (Run A décrypté)** : timeline de Run A = payload soumis à t≈0
(budget 5 s) mangé ; suffix soumis à t≈5 s **complété à t≈10 s**. Le ring se
ré-arme à ~10 s et complète tout transfert dont le budget enjambe ce moment.
Le payload de Run A n'est jamais arrivé (timeout expiré avant le
ré-armement) → MANIFEST sur buffer vide → pas de PWND. **L'adaptation
M-series est donc : payload soumis immédiatement avec un budget de 30 s.**
Ni gaster ni personne n'a ce cas : leurs hôtes n'ont pas de fenêtre
d'ingestion.

**Test** : plus aucun AbortPipeZero après le STALL de l'overwrite ; payload
mono-DNLOAD avec `USB_EP0_RECOVERY_TIMEOUT_MS = 30000` ; le reste (spray
async, CLR gaster) inchangé.

**Résultat** (`/tmp/tr4mpass-e6.log`) : le budget 30 s était bien appliqué ;
`payload DNLOAD ret=-7` quand même, **et** tous les transferts suivants
(suffix/zero-len/GETSTATUS) morts — contrairement à Run A où la suite passait.
**Falsifié** : le budget long ne suffit pas ; sans l'abort, la fenêtre ne se
referme jamais (observation ~55 s).

**Matrice finale recoupée** :

| Run | Abort | 1er post-STALL | 2e | Suite |
|---|---|---|---|---|
| Run A | 1× | IN 5 s mangé | OUT-multi 5 s mangé | **ring revenu**, MANIFEST OK, pas de PWND (payload jamais arrivé) |
| E2 | 1× | IN×5 mangés | — | mort |
| E3 | 1× | OUT simple ×3 | — | mort |
| E5b | 1× | OUT-multi 5 s | — | mort |
| E6 | **0** | OUT-multi 30 s | — | mort (~55 s) |

**Conclusion** : la séquence qui récupère = **abort + UN IN mangé + UN OUT
mangé**, et le payload mangé n'est jamais arrivé au device (d'où le MANIFEST
propre sur buffer vide de Run A) → après récupération il faut **re-envoyer**
le payload. E7 implémente exactement cette séquence.

### E7 — Séquence exacte de Run A + re-envoi du payload après récupération

**Hypothèse** : reproduire fidèlement la seule séquence de récupération
connue (abort → IN 5 s mangé → OUT-multi 5 s mangé), sonder GETSTATE
jusqu'au retour du ring, puis **re-envoyer** le payload en chunks 64 o
(pacing 2 ms, 2 rounds de secours avec 100 ms entre tentatives).

**Test** : implémenté dans `checkm8_patch.c` (Step 2/3) ; suffix/zero-len/
GETSTATUS inchangés après le re-envoi.

**Résultat** (`/tmp/tr4mpass-e7.log`) : **falsifié.** Deux tentatives
complètes, même tableau :

- `overwrite STALL confirmed` puis `AbortPipeZero ret=0x0` (immédiat) ;
- `recovery IN ret=-7` (mangé, 5 s) ;
- `recovery OUT ret=-7` (mangé, 5 s — le 528 o complet, comme Run A) ;
- `recovery probe [1..3/3] ret=-7` → `EP0 ring never recovered` → stage 4
  abandonné (garde à t+25 s).

Le préfixe Run A (abort + IN mangé + OUT mangé) est donc reproductible
**mais ne ré-arme pas le ring à lui seul**. Run A récupérait à ~10 s quand
le transfert suivant était le **suffix OUT 16 o** ; E7 meurt quand le
transfert suivant est une **sonde IN**. Nouveau décryptage candidat : après
abort + IN mangé + OUT mangé, **les OUT passent mais pas les IN** — et/ou
la récupération de Run A n'était pas due à notre séquence mais à un
auto-déblocage du ROM (~10 s, cf. hypothèse H-watchdog du §11/E8).

**Anomalie secondaire observée** (tentative 2) : après l'échec de la
tentative 1, le device est resté en state=0x05 puis a **disparu** du bus
(`No such device` ×10 pendant le stage 1) avant de revenir pour la
tentative 3. La séquence de récupération ResetDevice+ReEnumerate en
fin d'échec mérite un coup d'œil, mais c'est secondaire.

**En parallèle (contingence payload)** : comparaison ligne à ligne de
notre `assemble_payload` avec gaster — structure A9 (`pwnd[2]` + 11
champs), formule `payload_dest = boot_tramp_end - handler_sz -
sizeof(handler_tail)`, les 15 offsets S8000 (SRTG iBoot-2234.0.0.3.3) et
le tag ` PWND:[checkm8]` : **tout est identique**. L'hypothèse « payload
invalide » est écartée ; si un jour les octets arrivent, le contenu est
bon.

### E8 — Timeline de Run A, mais c'est le payload qui complète à t+10

**Découverte préalable (relecture gaster L996-1002, L1230-1236)** :
`dfu_send_data` et la boucle payload du stage patch envoient les données en
**plusieurs DNLOAD back-to-back** — le ROM **accumule** les DNLOAD dans le
même io_buffer (preuve : le setup de gaster soustrait ce qui a déjà été
envoyé, `config_overwrite_pad - transfer_ret.sz`). Pour A9 : 528 ≤ 2048 →
gaster fait UN SEUL DNLOAD 528 o, mais des chunks successifs reconstruisent
le même buffer.

**Décryptage E7** : après abort + IN mangé + OUT mangé, Run A enchaînait
des **OUT** (suffix 16 o soumis t+5, complété t+10 ; zero-len ; puis
GETSTATUS IN OK) alors que E7 mettait une **sonde IN** en premier → mort.
Règle candidate : après l'OUT mangé, le **premier transfert qui complète
doit être un OUT** ; les IN ne repassent qu'ensuite.

**Hypothèse E8** : reproduire la timeline de Run A mais faire porter le
rôle du « suffix qui complète à t+10 » au **payload lui-même** : après le
prefix Run A (abort + IN 5 s mangé + OUT 528 o 5 s mangé), envoyer le
payload en chunks 64 o (budgets 5 s, retry borné par chunk) SANS aucune
sonde IN. Le chunk dont le budget enjambe t+10 complète (comme le suffix
de Run A), le ROM accumule les chunks, le reste coule.

**Test** : remplacement du bloc sondes/re-envoi de E7 par la boucle chunks
directe (64 o, pacing 2 ms, max 6 essais/chunk, échec si le chunk 0
n'est jamais passé).

**Résultat** (`/tmp/tr4mpass-e8.log`) : **falsifié.** Après abort + IN
mangé + OUT mangé, les chunks OUT 64 o ne complètent jamais non plus
(6 × `-7`, ~30 s observés) → `chunk at 0 never completed`. Ce n'est pas
la direction ni le budget : **après le STALL de l'overwrite, le chemin
`DeviceRequestTO` (device user client) est mort quel que soit ce qu'on
lui envoie**, avec ou sans abort préalable. Convergence avec Run A :
son « récupération à ~10 s » n'était probablement pas une récupération
de ring pilotée par la séquence mais un auto-déblocage du chemin hôte ;
et les octets du payload n'y étaient pas arrivés.

### E9 — Changement de transport : interface user client (`ControlRequest`)

**Bascule méthodologique (directive utilisateur)** : arrêter de deviner
à partir du code source, aller chercher la documentation — papier de
recherche DSEC/Habr, code ipwndfu original, writeups, et le code des
outils qui MARCHENT sur Apple Silicon. Résultats (§12) :

1. **DSEC/Habr (analyse technique checkm8)** : le `stall` est annulé
   côté OS *pendant qu'il est dans la queue du device* — ensuite « le
   device continue de recevoir tous les SETUP et les place dans sa
   queue d'exécution ». Après le STALL de l'overwrite, **le device est
   sain** ; il n'y a RIEN à « récupérer » côté device. Le wedge que
   nous observons est donc **côté hôte**.
2. **ipwndfu (axi0mX, original)** : après l'overwrite, il enchaîne
   SANS vérifier aucun résultat : DNLOAD payload 0x800, DNLOAD suffix,
   zero-len, GETSTATUS ×3 (non vérifiés), puis `usb_reset`. Pas un
   seul abort, pas une seule sonde, pas un seul re-envoi.
3. **palera1n README (officiel)** : « USB-C port on Apple Silicon Macs
   may require manual unplugging and replugging of the lightning cable
   **after checkm8 exploit** » → leur problème M-series est l'APRÈS-
   exploit (envoi de PongoOS au DFU pwned), pas l'exploit lui-même.
4. **checkra1n (annonce M1 officielle, avril 2021)** : le bug USB stack
   M-series qui plante des devices ne concerne que **A7, A9X, A10,
   A10X** — **l'A9 n'est PAS concerné** → checkra1n fait passer
   l'exploit A9 sur M-series USB-C sans replug. Le transport qui
   marche existe.
5. **palera1n-c `src/usb_iokit.c` (dérivé pongoOS/checkra1n)** : le
   transport de production checkra1n sur Apple Silicon n'est PAS
   `DeviceRequestTO` sur le user client du device (celui de gaster et
   le nôtre) mais **`ControlRequest(handle, 0, &req)` sur l'interface
   user client** (`IOUSBInterfaceInterface`, pipe 0) avec un
   `IOUSBDevRequest` **sans timeout**. Chemin kernel différent.
6. **gaster IOKit re-lu précisément** : `send_usb_control_request`
   sync = `DeviceRequestTO` 5 ms, retour TOUJOURS true, résultat
   ignoré par le stage patch (payload NULL). L'async (abort en flight)
   n'est utilisé QUE pour stall/leak/no_leak. Sur Intel ça marche ;
   sur notre M-series, le MÊME chemin wedge après le STALL.

**Hypothèse E9 (nouvelle)** : le wedge EP0 post-STALL est une propriété
du **chemin device user client** (`IOUSBDeviceInterface320::
DeviceRequestTO`) sur Apple Silicon — ses timeouts TO + sa gestion du
STALL laissent le pipe dans un état bloquant en interne. Le chemin
**interface user client** (`IOUSBInterfaceInterface::ControlRequest`,
celui de pongoOS/checkra1n, sans timeout) n'a pas ce comportement : il
doit passer après l'overwrite STALL sans abort ni attente.

**Test** : nouveau transport dans `iokit_usb.c`
(`iokit_usb_iface_activate()` + `iokit_usb_ctrl_transfer_iface()`),
branché dans le stage 4 : overwrite via device user client (marche,
STALL confirmé à chaque run), puis payload 528 o + suffix + zero-len +
GETSTATUS ×3 via **interface user client**, sans abort ni sonde, puis
ResetDevice + ReEnumerate comme avant.

**Résultat** (`/tmp/tr4mpass-e9.log`) : **falsifié en l'état, mais
nouveau signal.** L'ouverture de l'interface user client réussit
(`interface user client opened`), mais **chaque** `ControlRequest`
renvoie `0xe0004051` (payload, suffix, zero-len, 3× GETSTATUS —
régulier, pas un wedge aléatoire). Lecture : le device user client
reste ouvert en même temps (USBDeviceOpen + SetConfiguration) —
palera1n-c, lui, n'ouvre **QUE** l'interface user client. Conflit
d'accès exclusif probable entre les deux user clients. **E10** =
passer tout le stage 4 (ou tout le run) par l'interface seule :
fermer le device user client avant d'ouvrir l'interface, ou n'ouvrir
que l'interface dès le départ.

### Tableau récapitulatif des runs stage 4

| Run | Abort post-STALL | Transport post-STALL | Ring après STALL | Suite |
|---|---|---|---|---|
| Run A | 1× | sync OUT suffix 16 o → complété ~t+10 s | récupéré ~10 s | MANIFEST OK, **pas de PWND** (payload jamais arrivé) |
| Run B | 2× | sync | mort | tout `-7` |
| canary | 1× | sync IN | mort (IN 500 ms puis 5 s ×7) | tout `-7` |
| E2 | 1× | sync IN×5 | mort | tout `-7` |
| E3 | 1× | sync chunks OUT | mort (OUT simple ×3) | garde coupe à 15 s |
| E5b | 1× | sync mono-DNLOAD | mort | tout `-7` |
| E6 | 0 | sync mono-DNLOAD 30 s | mort (~55 s) | tout `-7` |
| E7 | 1× | sync IN mangé, OUT mangé, **sondes IN** | mort (sondes `-7` ×3) | garde coupe à t+25 s |
| E8 | 1× | sync IN mangé, OUT mangé, **chunks OUT 64 o** | mort (chunks `-7` ×6) | garde coupe à ~30 s |
| **E9** | **0** | **interface user client `ControlRequest`** (device user client encore ouvert) | interface ouverte mais tout `0xe0004051` | E10 : interface SEULE (fermer le device client) |
| **E10** | **0** | **stage 4 tout-libusb** après spray IOKit | overwrite `-9` propre puis **tout `-7`** ; SET_FEATURE halt EP0 hôte | E11 : forme overwrite `0x00/0x00` |
| **E11** | **0** | **overwrite `0x00/0x00`** (openra1n), reste gaster-exact DeviceRequestTO | séquence complète : overwrite `-9`, payload **528 OK**, suffix 16, zero-len 0, MANIFEST 0x06→0x07→0x08 — mais **pas de PWND** (callback muet ou overwrite jamais atterri ?) | E11b : log `wLenDone` du STALL |
| **E11b** | **?** | idem E11 + `wLenDone` du STALL loggé | `wLenDone=0` au STALL (ambigu : IOKit ne met peut-être pas à jour le champ au STALL) | E12 : wIndex 0x80→0x00 |
| **E12** | **0** | **overwrite openra1n/ipwndfu exact** (0x00,0,0,**0x00**,48) | signature identique à E11 (pas de PWND) ; SetConfiguration(1) confirmé chez gaster L335 + palera1n L143 | E13 : skip CLR_STATUS stage 3 (env `TR4MPASS_SKIP_CLRSTATUS=1`) |
| **E13** | **?** | **spray sans CLR_STATUS** (pas de wedge `0xe00002ed`/ré-énumération avant l'overwrite) | payload **meurt** `-7`×6 sur le handle d'origine après STALL overwrite → TOUT STALL wedge le handle courant (lecture E10 corrigée) | E14 : reconstruire le handle entre overwrite et payload |
| **E14** | **?** | E13 + **reconstruction du handle IOKit** entre overwrite et payload | payload `-7`×6 — la ré-activation révèle que **le DEVICE ré-énumère** après le STALL (pas juste l'hôte) | E15/E16 |
| **E15** | — | `shim.c` libopenra1n (piste interface client) | **éliminée** : il appelle `DeviceRequestTO`, même transport que nous | — |
| **E16** | **✅** | **gaster de 0x7ff compilé et exécuté** (implémentation de référence, payloads A9 officiels) | **PWND:[checkm8] OBTENU** — câble direct, sans hub, sans replug → notre port a un défaut précis | E16bis : diff structurel → E17 : trace gaster |
| **E16bis** | — | diff notre stage 3/4 ↔ gaster gagnant (code + dump config) | table 0x8000 + payload 528 identiques ; 2 diffs : séparation stricte des stages (ResetDevice+ReEnum+handle neuf) ; overwrite **0x02/0x03** avec data 48 (règle 3 falsifiée) | E17 |
| **E17** | **?** | trace requête-par-requête du gaster gagnant (env `GASTER_TRACE`) | *en attente : device à remettre en DFU normal* | E18 |
| **E18** | **⚠️** | portage gaster-exact (overwrite 0x02/0x03, pas de rebuild, reset inter-stage, timeouts 5 ms) | overwrite STALL **puis callback EXÉCUTÉ** (device rebooté hors DFU au reset bus — jamais vu avant) ; payload data phase jamais partie (`-7`×6, wLenDone=0) ; blobs byte-exact upstream | E17 : trace gaster gagnant |
| **E19** | **✅** | P5=H1 + portage P3 + orchestration gaster-exact (IOKit dès stage 1, reset ×3 + final, verify IOKit) | **PWND:[checkm8]** — câble direct, sans hub, sans replug ; attempt 3 `sent=0`/pad 1280 (artefacts) mais STALL corrects partout ; device resté en DFU pwned | E20 |
| **E20** | **?** | préparation étape [3/8] : `scripts/{ipsw_pull.py,detect_ios.sh,ramdisk_test_ibss.sh}` + découverte SRTG=BootROM (≠ iOS) | outils testés — **run iBSS en attente** | run transition DFU→recovery |

### E20bis — Analyse palera1n v3.0.0-beta.3 (clone /tmp/palera1n-check)

**Pourquoi le clone** : vérifier si palera1n contient des images
prêtes (iBSS/iBEC/ramdisk/pongo) réutilisables. **Réponse : NON** —
`Pongo.bin` et `ramdisk.dmg` sont des **artefacts CI**
(`.github/workflows/build.yml`, jobs `build-pongo`/`build-ramdisk`),
pas dans le repo. Mais la lecture du source révèle un **choix
d'architecture différent du nôtre**, avec un impact direct sur le
risque USB M-series.

**Architecture palera1n = pongoOS-as-payload (1 seule reconnexion)** :

1. checkm8 classique (libopenra1n, 1stparty/, MIT) jusqu'au patch ;
2. le payload du stage 4 n'est PAS un petit shellcode : c'est
   **pongoOS** compressé LZ4, encapsulé par `generate_stage1`
   (stage1.c) avec `lz4dec.bin` (500 o) comme décompresseur ;
3. l'overwrite déclenche : lz4dec → pongoOS → le device re-énumère
   **PID 0x4141 (pongoOS)** — pas de mode recovery 0x1281, pas d'iBSS,
   pas d'iBEC, pas de devicetree IPSW ;
4. TOUT le reste (kpf, ramdisk, boot, shell) passe par le shell
   pongoOS sur USB (`pongo_helper.c`, 295 lignes : commandes + upload
   de fichiers + `bootx` depuis pongo).

**Pour l'A9 (0x8000)** : `yolo_s8000.bin` (2780 o, YoloDFU stage-1
loader) est EMBARQUÉ dans le repo ; `generate_stage1` prend le chemin
**non-wnx** pour A9 (A9X/A10/A10X/A11/T2 sont wnx — pas nous) →
presize 0xc0, plus simple. Compatible avec notre CPID.

**Détail important** : palera1n **REFUSE les devices pwned DFU**
(exploit.c L381-387 « Detected pwned DFU mode device, please enter
normal DFU mode ») — son flux fait pwn+pongo en un seul passage.
Notre flux en 2 étapes (pwn puis post-exploit séparé) reste valable
mais doit piloter pongoOS après notre propre pwn.

**Comparaison des deux routes pour path_a** :

| | Route IPSW (notre path_a actuel) | Route pongoOS (palera1n) |
|---|---|---|
| reconnexions USB | **3** (DFU→recovery→iBEC→recovery) | **1** (DFU→pongoOS) |
| risque M-series USB-C | élevé (zone documentée palera1n) | minimal (même mécanique que le pwn) |
| images nécessaires | iBSS+iBEC+devicetree+ramdisk+trustcache (IPSW + pré-traitement) | Pongo.bin (CI artifact, à obtenir/construire) + ramdisk custom |
| code à écrire | 0 (path_a_ramdisk.c complet déjà écrit) | client pongoOS (~300 lignes, modèle pongo_helper.c) + stage1 dans checkm8_patch |
| ramdisk | extrait IPSW + patches kernel + remplacement mobileactivationd | ramdisk custom palera1n (contient déjà ce qu'il faut, adapter à notre but) |

**Stratégie retenue** : le test A (iBSS) reste la **mesure
discriminante** — si la transition DFU→recovery passe sans replug, la
route IPSW (déjà écrite) est la plus courte. Si elle échoue, pivot
route pongoOS (une seule transition, la même qui marche déjà en E19).
Le test A coûte 2 minutes et décide.

| **E20bis** | **ℹ️** | analyse palera1n v3.0.0-beta.3 : route pongoOS (1 reconnexion) vs route IPSW (3 reconnexions) ; yolo_s8000.bin embarqué ; Pongo.bin/ramdisk.dmg = artefacts CI | décision repoussée au résultat du test A | run iBSS |

**E20bis-addendum — palera1n ne fait AUCUNE activation** (grep exhaustif
sur tout l'arbre : 0 occurrence de « activation », « hacktivation »,
« setup.app », « mobileactivation »). Ce que palera1n fait réellement,
en 3 couches :

| Couche | Où | Persiste au reboot |
|---|---|---|
| patches kernel (KPF : AMFI, sandbox, signatures) | **RAM seulement** | ❌ non |
| bootstrap (Sileo, tweaks) | 2ᵉ volume APFS « fakefs » (iOS 15+) ou `/var/jb` | ✅ oui |
| app palera1nLoader | `/Applications` | ✅ oui |

Séquence réelle (`pongo_helper.c::send_full_pongo_jailbreak`) :
`fuse lock` → `sep auto` → upload KPF → `modload` → upload ramdisk →
`ramdisk` → upload binpack → `overlay` → `xargs <boot_args>` →
`bootx`. **Aucune modification d'iOS sur disque** : le kernel réel
démarre avec des patches appliqués en RAM. D'où le caractère
semi-tethered (relancer l'outil après chaque reboot).

⇒ Pour notre objectif : un device **non activé** (bloqué sur Setup)
le reste après palera1n. palera1n ne fournit ni record d'activation,
ni remplacement de mobileactivationd, ni deletescript, ni préservation
du Signal. **Seul emprunt possible : le transport USB** (1 transition
au lieu de 3). Tout le pipeline [4/8]→[8/8] reste du code tr4mpass.

### Règles M-series validées empiriquement (l'état de l'art actuel)

1. Spray : transport **async + abort en flight** (1 ms), jamais synchrone ;
   sémantique de succès = `sz == 0`, pas le statut de complétion.
2. CLR_STATUS final : 193 zéros, 5 ms, **résultat ignoré** ; le wedge
   `0xe00002ed` + ré-énumération qu'il provoque est sans conséquence
   (gaster fait ResetDevice+ReEnumerate après CHAQUE stage — gaster_ref
   L1285 — et le heap device-side survit ; confirmé : le STALL de
   l'overwrite revient après la ré-énumération).
3. **La requête qui tue EP0 hôte est SET_FEATURE(ENDPOINT_HALT)
   (0x02/0x03)** — elle pose le bit halt sur EP0-IN côté contrôleur ;
   ensuite TOUT transfert meurt en timeout (`-7`), via libusb comme
   via DeviceRequestTO, direction et budget sans importance (E2..E8,
   E10). Un rejet simple (`0x00/0x00`) STALLe SANS poser le halt :
   EP0 survit (stage 2 : `-9` puis `ret=64` OK sur le même handle).
   ⇒ L'overwrite doit utiliser la forme **0x00/0x00** (ipwndfu,
   openra1n), pas la forme gaster 0x02/0x03.
4. Le contenu du payload n'est PAS le problème : `assemble_payload` est
   identique octet-pour-octet en structure et en offsets à gaster (§11/E7).

---

## 12. Sources de la recherche documentaire (bascule E9)

### 12quater. Options « bas niveau » évaluées (demande utilisateur, E16)

Question posée : « peut-on bypasser en ultra low level — assembleur
compilé pour remplacer une lib, ou utiliser directement le code
palera1n ? »

| Levier | Verdict | Raison |
|---|---|---|
| **Écrire le transport en assembleur** | **inutile** | Le blocage n'est pas du calcul mais un **routage noyau** : quel user client (`IOUSBDeviceInterface` vs `IOUSBInterfaceInterface` vs `IOUSBHostDevice`) reçoit le transfert. Aucune instruction assembleur ne change la pile kernel qui décide du reset de port. |
| **Utiliser gaster (0x7ff) tel quel** | **fait** (E16) | Source identique à `gaster_ref.c` étudié depuis le début, payloads A9 officiels. Binaire compilé dans `~/Documents/git/github/maxime1907/gaster/gaster`. Test en attente du device. |
| **Utiliser palera1n V3.X / libopenra1n** | **non viable** | (a) `checkm8.c` a un bug de boucle payload (`bool ret = false` jamais mis à true) → n'envoie jamais le payload ; (b) transport identique au nôtre (`DeviceRequestTO`, shim.c) ; (c) payload A9 = yolo DFU (PID 0x4141), architecture différente de gaster. |
| **Transport IOUSBHost (framework moderne)** | **levier réel, non testé** | `IOUSBHost.framework` (SDK 27.2 présent) expose `IOUSBHostDevice sendDeviceRequest:data:bytesTransferred:completionTimeout:error:` — chemin **différent** du `IOUSBDeviceInterface320` d'IOUSBLib qu'on utilise depuis E5. C'est le candidat n°1 si E16 montre que gaster échoue aussi : même rôle, autre implémentation kernel. |
| **libciderra1n (checkra1n)** | **inaccessible** | Fermé (binaire), c'est ce que palera1n appelle sur macOS pour A9-A11. |

**Ordre de priorité** : E16 (gaster) d'abord — une seule exécution
tranche « notre code vs la plateforme ». Puis IOUSBHost si besoin.

### 12ter. Mécanique du STALL d'overwrite (agents DSEC/Habr/ipwndfu, E11)

- **Ce que fait le device au STALL** : au SETUP, le handler DFU stocke
  `ep0_data_phase_length` + `ep0_data_phase_buffer` et « avance
  aveuglément » les pointeurs — tout paquet OUT arrivant est memcpy
  dans le buffer, sans re-validation (littlelailo, Habr/DSEC « Data
  Stage »). Après le UAF, le buffer périmé = io_buffer libérée : les
  paquets OUT de l'overwrite sont ACKés et DMAés dedans, puis la
  requête est rejetée → l'hôte observe STALL (littlelailo ; King ;
  gaster exige `USB_TRANSFER_STALL`).
- **Le STALL SecureROM est différé, pas immédiat** (Habr, commentaires
  a1exdandy) : « le bus USB ne passe pas en STALL (les paquets
  handshake STALL ne sont pas envoyés, seulement des NACK) » et le
  contrôleur « traite toujours les paquets SETUP ». Le STALL observé
  par l'hôte = rejet logique de la requête, pas un endpoint figé.
- **0x02/0x03 SET_FEATURE(ENDPOINT_HALT) vs 0x00/0x00** : SET_FEATURE
  demande au device d'halter son PROPRE EP0-IN (malformed avec data
  phase) ; 0x00/0x00 = GET_STATUS dans le mauvais sens — requête
  invalide rejetée dont la data phase atterrit quand même dans le
  buffer périmé, sans poser le feature halt. ipwndfu upstream :
  `libusb1_no_error_ctrl_transfer(device, 0, 0, 0, 0, overwrite, 100)`
  (bmRequestType=0, bRequest=0, errections ignorées). NB : ipwndfu
  upstream n'a PAS de chemin A9/S8000 (futur) ; la variante A9 vient
  des forks (King `checkm8_A8_A9`, overwrite {0,0,0,0,0x180380000,0}).
- **Côté hôte macOS** : libusb darwin ne fait AUCUN CLEAR_FEATURE
  automatique après un STALL de control transfer (seul le chemin
  cancel fait AbortPipe ; ClearPipeStallBothEnds seulement pour
  IOUSBFamily ≤ 245). Ni ipwndfu ni gaster n'appellent clear_halt —
  ils comptent sur le device qui ré-arme EP0 au prochain SETUP.
- **checkra1n M1 (officiel)** : la pile USB-C des Apple Silicon fait
  un DOUBLE reset USB sur USBReEnumerate (pas USB-A) ; avec EP0_IN
  stallé et des paquets leakés en file, ça déclenche un NULL-deref →
  crash/reboot du device sur A7/A9X/A10/A10X (A9 non listé). Workaround
  checkra1n : sauter cette étape + replug manuel. palera1n README :
  USB-C AS peut exiger un replug, hub recommandé.
- **Jamais publié** : comportement AS-XHCI vs Intel pour les STALL de
  control transfer ; `wLenDone` au STALL (aucun implémenteur ne le
  lit) ; nos observations E10/E11 restent des hypothèses locales.

### 12bis. Analyse du clone palera1n PR #765 (V3.X, sha 42d6506) — graphify + lecture

Repo cloné dans `~/Documents/git/github/maxime1907/palera1n-pr765`, graphe
graphify dans `palera1n-pr765/graphify-out/` (1 076 nœuds, 2 151 arêtes,
62 communautés ; HTML `graph.html`).

**Architecture V3.X** : `src/exploit.c` pilote deux handles (DFU 05ac:1227,
pongo 05ac:4141) dans une boucle infinie ; machine à états RESET/SETUP/
SPRAY/PATCH/YOLODFU ; **`reset_usb_handle()` (ResetDevice+ReEnumerate)
entre CHAQUE étape** — l'handle est rouvert à chaque stage, rien ne
traverse les stages. Le retour de PATCH est **ignoré** (« Checkmate? »).

**libopenra1n `checkm8.c` stage patch (A9)** — différences majeures vs gaster :

1. **Pre-overwrite différent** : `checkm8_usb_request_stall()` (SET_FEATURE
   sync) + `checkm8_usb_request_leak()` (async 1 ms), puis **2×
   SET_FEATURE(ENDPOINT_HALT)** de plus, puis overwrite.
2. **Forme de l'overwrite** : `send_usb_control_request(handle, 0x00, 0,
   0, 0x00, overwrite, 48)` — bmRequestType=**0x00**, bRequest=**0**
   (comme ipwndfu original ; gaster utilise 0x02/3 SET_FEATURE). STALL
   requis.
3. **Overwrite A9 : `callback.next = insecure_memory_base`** (callback
   nul !) pour TOUS les 64-bit — gaster A9 met `callback = insecure_mem`.
   Architecture payload différente : stage1 A9 = chaîne ROP de 0xc0
   (func_gadget 0x10000de0c, write_prim2, dc_civac, icache — PAS de
   ttbr0/tlbi) + payload **yolo_s8000.bin** embarqué (YoloDFU : le device
   ré-énumère en PID 0x4141). Approche « yolo DFU » ≠ gaster (PWND serial).
4. **BUG dans la PR** : `bool ret = false` n'est jamais mis à true →
   `for (i = 0; ret && i < data_sz; ...)` — **la boucle payload est morte**
   ; stage patch n'envoie JAMAIS le payload, retourne TRANSFER_FAILED,
   que le driver ignore. Le chemin openra1n V3.X ne peut pas pwned en
   l'état (WIP) — cohérent avec le fait que macOS/Linux utilisent
   **libciderra1n** (fermé, checkra1n 1337) et openra1n seulement iOS.
5. **Interface user client** : `send_interface_control_request` /
   `WritePipe` ne servent QUE dans `pongo_helper.c` (upload pongo APRÈS
   l'exploit). Le flow checkm8 n'utilise JAMAIS l'interface client →
   la prémisse E9 (« le transport checkra1n = interface client ») était
   fausse ; le transport checkm8 de checkra1n (ciderra1n) est fermé.
6. USB_TIMEOUT = 5 partout, pas d'override ; `usb_handle_t` porte device
   ET interface clients ouverts simultanément (USBDeviceOpenSeize +
   USBInterfaceOpen) → l'hypothèse « conflit exclusif » de E9 est fausse
   aussi ; le 0xe0004051 d'E9 reste inexpliqué (peut-être interface
   déjà réclamée par le device client ouvert, ou pipe 0 de l'interface
   non armé).

**LE indice transversal (nos propres logs, stage 2)** : le padding
1280 o via **libusb** STALLE (`ret=-9`) puis le DNLOAD 64 o suivant passe
(`ret=64`) sur le MÊME handle — **libusb/darwin survit à un STALL avec
data phase sur M-series** ; c'est `DeviceRequestTO` (device user client)
qui wedge. Or le stage 4 n'a besoin d'AUCUN abort (ipwndfu/gaster/
openra1n : zéro abort post-overwrite) — les aborts étaient le motif du
passage en IOKit, pas les STALL.

### E10 — Stage 4 entièrement via libusb (le chemin qui prouve survivre aux STALL data-phase)

**Hypothèse** : après le spray IOKit (async+abort nécessaire), rouvrir un
handle **libusb** et faire TOUT le stage 4 en libusb : overwrite SET_FEATURE
48 o (STALL `-9` attendu, sans conséquence), payload 528 o, suffix 16 o,
zero-len, GETSTATUS ×3, puis reset (IOKit ResetDevice). Le STALL du
stage 2 (data phase + STALL puis transfert OK) prouve que libusb/darwin
ne wedge pas sur ce motif.

**Test** : implémenté — `dfu_deliver` (`checkm8.c`) désactive IOKit
juste après le spray (`iokit_usb_deactivate()`, fermeture user client
SANS reset bus → le heap sprayé survit) et rouvre un handle libusb
(`usb_dfu_find`), puis `checkm8_stage_patch` (`checkm8_patch.c`) route
tout le stage 4 vers le chemin libusb commun (helper
`stage4_libusb_path` : overwrite SET_FEATURE 48 o → STALL `-9` attendu,
payload 528 o en chunks, suffix 16 o, zero-len, GETSTATUS ×3,
`libusb_reset_device`). Le fallback IOKit historique reste disponible
si aucun handle libusb n'est disponible.

**Résultat** (`/tmp/tr4mpass-e10.log`, 3 tentatives identiques) :
**falsifié, mais la run contient la découverte déterministe manquante.**

Faits (identiques aux 3 tentatives) :

1. **L'overwrite via libusb STALL et libusb le rapporte
   proprement** (`ret=-9` PIPE, pas de wedge hôte) — le transport
   libusb survit au STALL data-phase comme prédit.
2. **Tout ce qui suit l'overwrite time out** (`-7` ×6 : payload 528,
   suffix, zero-len, 3× GETSTATUS) via libusb, sur les trois
   tentatives — le payload n'est pas parti.
3. Le device survit : série propre, DFU état 0x02, réutilisable.
4. **Le device ré-énumère (addr 1→2) au CLR_STATUS du stage 3**
   (wedge `0xe00002ed`), juste avant le patch.

**L'expérience naturelle dans le même log** : les DEUX requêtes
STALLent, mais seule l'une tue EP0 :

| | stage 2 padding | stage 4 overwrite |
|---|---|---|
| Requête | `0x00/0x00`, 1280 o | `0x02/0x03` SET_FEATURE(ENDPOINT_HALT), 48 o |
| STALL | `-9` | `-9` |
| Suivante | **`ret=64` OK** | **`-7` ×6, tout meurt** |

Lecture : **SET_FEATURE(ENDPOINT_HALT) pose le bit halt sur EP0-IN
côté hôte** (fonction standard : feature reçu par le device, mais le
contexte d'endpoint du contrôleur reste halté — plus aucun TD soumis
→ timeouts, quel que soit le user client ou libusb). Un rejet simple
(`0x00/0x00`) STALLe **sans** poser le halt → EP0 reste vivant.
C'est exactement la forme d'overwrite d'**ipwndfu** et d'**openra1n**
(`send_usb_control_request(handle, 0x00, 0, 0, 0x00, overwrite, 48)`)
; gaster (0x02/0x03) marche sur Macs Intel où la pile USB nettoie le
halt autrement. Le STALL de l'overwrite était donc aussi un faux
positif : il arrive aussi sur un device frais.

**E11** = une seule variable : la **forme de l'overwrite**
`0x00/0x00` au lieu de `0x02/0x03` (openra1n/ipwndfu), tout le reste
gaster-exact (ordre, DeviceRequestTO, fire-and-forget, reset final).
Écart assumé vs gaster, justifié par le log E10. Revert de la
bascule libusb (E10) dans `dfu_deliver` ; `stage4_libusb_path`
conservé pour Linux/Windows (`__attribute__((unused))` sur macOS).

### E11 — Overwrite forme 0x00/0x00 (openra1n/ipwndfu), reste gaster-exact

**Hypothèse** : seule la forme SET_FEATURE(ENDPOINT_HALT) wedge EP0
hôte (halt feature posé côté contrôleur). La forme `0x00/0x00`
rejette la requête STALL sans halt → EP0 survit → payload/finalize
partent → MANIFEST → PWND.

**Test** : implémenté — `checkm8_patch.c` : overwrite
`IOKIT_XFER(0x00, 0x00, 0, 0x80, &ow, 48, 5)` ; prelude TLBI gardé
(gaster 1215, sauté pour A9) ; payload 528 o en UN DNLOAD + suffix +
zero-len + 3× GETSTATUS via DeviceRequestTO, résultats loggés et
ignorés ; ResetDevice + ReEnumerate finaux. Prime/GETSTATE/abort et
interface user client supprimés (tout ce qui subsistait des E2..E9).
`dfu_deliver` ne bascule plus libusb après le spray (revert E10).

**Résultat** (`/tmp/tr4mpass-e11.log`) : **la séquence passe
intégralement pour la première fois** (3 runs ; 2/3 tentatives
complètes, 1 avortée au overwrite par `0xe00002d8` NotReady résiduel
du wedge CLR_STATUS) :

```
overwrite 0x00/0x00 : ret=-9 (STALL)      <- EP0 SURVIT au STALL
payload  DNLOAD 528 : ret=528  (accepté !)
suffix   DNLOAD  16 : ret=16   (accepté)
zero-len DNLOAD   0 : ret=0    (accepté)
GETSTATUS[0..2]     : 0x06 -> 0x07 -> 0x08 (walk MANIFEST complet)
ResetDevice+ReEnum  : ok
```

Mais **pas de PWND** dans la série. Le walk MANIFEST 0x06→0x07→0x08
s'est fait, le device est resté en DFU avec une série propre : le
callback hijacké n'a PAS modifié `gUSBSerialNumber`.

Deux lectures possibles, discriminées par `wLenDone` du STALL
d'overwrite (logging ajouté après coup — E11b) :

- **H1 (données perdues)** : STALL avec `wLenDone=0` → la DATA phase
  n'a pas atterri (device a rejeté avant de mapper le buffer) →
  overwrite jamais écrit → payload alloué dans un buffer DFU normal →
  walk MANIFEST d'un download propre, sans shellcode.
- **H2 (données posées, payload muet)** : STALL avec `wLenDone=48` →
  overwrite atterri mais l'architecture gaster (UAF io_request + ROP
  TLBI au MANIFEST) ne déclenche pas sur ce chemin device/hôte.

Vérification sources (sous-agents, §12bis/12ter) : gaster ne lit
JAMAIS `wLenDone` (seul `.ret` compte) — aucun artefact des
implémenteurs sur ce que vaut `wLenDone` au STALL. openra1n exige
`USB_TRANSFER_STALL` sur 0x00/0x00 puis enchaîne sans vérifier les
octets. Aucun CLEAR_FEATURE chez gaster/openra1n/ipwndfu (le device
ré-arme EP0 tout seul au prochain SETUP — Habr : le SecureROM
« always processes SETUP packets », STALL = rejet logique, pas un
handshake qui fige l'endpoint).

**E11b** (`/tmp/tr4mpass-e11b.log`) : re-run E11 avec `wLenDone`
loggé sur le STALL d'overwrite (chemin direct ET post-réactivation).

**Résultat** : `wLenDone=0` **partout** — sur le STALL d'overwrite
48 o (les deux chemins) ET sur le SET_FEATURE du spray (attendu :
wLength=0). **Ambigu** : rien ne prouve qu'IOKit met à jour
`wLenDone` au STALL (gaster ne lit jamais ce champ ; le STALL de
l'overwrite n'a pas d'équivalent lisible côté libusb). Le
discriminant H1/H2 n'est donc PAS tranché — wLenDone=0 peut être un
artefact IOKit autant qu'une vérité device.

**Nouvelle divergence trouvée en relisant openra1n/ipwndfu** (agent
sources) : leur overwrite est `(0x00, 0x00, wValue=0, wIndex=0x00,
48)` — **wIndex=0x00**. Notre E11 a envoyé `wIndex=0x80` (hérité de
la forme gaster SET_FEATURE où wIndex=endpoint). Pour une requête
invalide 0x00/0x00, wIndex n'a pas de sens sémantique, mais le
SecureROM peut rejeter différemment selon wValue/wIndex (validation
précoce au SETUP → STALL sans data phase → wLenDone=0 serait alors
la vérité device = H1).

**E12** = overwrite **exactement openra1n/ipwndfu** :
`(0x00, 0x00, 0, 0x00, 48)` — une seule variable (wIndex 0x80→0x00).

**Résultat** (`/tmp/tr4mpass-e12.log`) : **falsifié** — signature
identique à E11 (overwrite `-9` wLenDone=0, payload 528 accepté,
MANIFEST complet, pas de PWND ; 2 tentatives complètes, 1 avortée
`0xe00002d8` — le retry NotReady n'était pas encore compilé dans ce
binaire). wIndex ne change rien.

Au passage : `SetConfiguration(1)` à l'activation IOKit est bien
présent chez gaster (L335) ET palera1n (L143) — pas une divergence.
L'AbortPipeZero d'entrée de stage 3 reste une divergence mineure
(références : seulement dans le chemin async+abort).

**E13** = sauter le **CLR_STATUS du stage 3** (env
`TR4MPASS_SKIP_CLRSTATUS=1`). Raison : le wedge `0xe00002ed` +
ré-énumération qu'il provoque sur M-series est le dernier événement
device-visible avant l'overwrite ; ipwndfu n'envoie JAMAIS de
CLR_STATUS dans le chemin A9 (stall→leak→no_leak→usb_reset→
overwrite→payload→usb_reset).

**Résultat** (`/tmp/tr4mpass-e13.log`) : **tranchant.** Sans le
wedge CLR, l'overwrite STALLe (`-9`, wLenDone=0) sur le handle
d'origine — et **le payload DNLOAD meurt** (`-7` ×6 : payload,
suffix, zero-len, 3× GETSTATUS), sur les 3 tentatives (dont 2 avec
un STALL réel au stage 2, pas le timeout-as-stall).

**Synthèse révisée (importante)** : je relis les logs passés —
l'« expérience naturelle » d'E10 était **fausse** : les attempts 1-2
du stage 2 (dnload `sent=2048` puis `ret=64`) n'ont JAMAIS STALLé
(complétion normale avant abort) ; le STALL du padding n'arrive qu'à
l'attempt 3, suivi d'une fermeture du handle. Donc :

- **TOUT STALL EP0 wedges le handle courant** (libusb ET
  DeviceRequestTO), quelle que soit la forme (0x00/0x00 comme
  0x02/0x03) — le « libusb survit au STALL » d'E10 était une mauvaise
  lecture.
- E11/E12 : le payload était accepté (528) **uniquement sur un handle
  fraîchement ré-activé** (le NoDevice avait déjà forcé une
  réactivation AVANT le STALL de l'overwrite) — c'est-à-dire sur un
  device fraîchement énuméré → cohérent avec H1 (device DFU normal,
  overwrite jamais atterri, walk MANIFEST d'un download propre).
- E13 prouve que sur le handle NON ré-énuméré, après le STALL
  d'overwrite, plus rien ne passe.

**E14** = E13 + **une seule variable** : reconstruire le handle IOKit
entre l'overwrite et le payload (deactivate + activate — SetConfig
comme gaster L335). L'état device-side survit à la ré-énumération
hôte (l'overwrite STALLE encore après le re-enum du CLR en E11/E12).
Si payload accepté + PWND → win. Si accepté sans PWND → overwrite
n'atterrit pas → E15 = même chose avec la forme gaster 0x02/0x03
(littlelailo : « set configuration handler ignores wLength » — la
data phase atterrit via le handler hacké, ce que 0x00/0x00 ne
garantit pas).

**Résultat** (`/tmp/tr4mpass-e14.log`) : **falsifié** — payload
`-7` ×6 après reconstruction du handle, 3/3 tentatives. MAIS le log
contient le fait décisif :

```
overwrite → STALL (-9)
iokit_usb: deactivated
iokit_usb: DFU service re-acquired after 2 polls (250 ms)
           -- device re-enumerated      <-- on n'a fermé QUE notre client
```

**Le DEVICE ré-énumère de lui-même après le STALL d'overwrite.** Ce
n'est donc pas le host pipe qui est wedgé : le device reset sa pile
USB. Toute la séquence suivante (payload accepté en E11/E12, ou
timeout en E13/E14) se joue sur un device **frais** → cohérent avec
l'absence de PWND dans TOUS les runs depuis E5b.

Reste à expliquer pourquoi le device re-enumère. Deux candidats :
(a) le STALL EP0 déclenche côté hôte un port reset (IOKit) — il faut
alors un transport qui ne resette pas le port ; (b) le SecureROM
crashe sur la requête (double-reset USB-C Apple Silicon, note
checkra1n M1) — mais A9 n'est pas dans la liste des puces touchées.

**E15 (piste)** : relire `shim.c` de libopenra1n — l'agent n°3 avait
tronqué la réponse exactement sur `send_usb_control_request_no_data`,
et libopenra1n ouvre le user client **interface** EN PLUS du device
client. Si leur flow checkm8 route les transferts par
`IOUSBInterfaceInterface::ControlRequest` (pipe 0 de l'interface), le
STALL n'implique peut-être pas le port reset du device client.

**Résultat piste shim.c** : **éliminée.** `shim.c` (62 lignes, lu en
entier) : `send_usb_control_request_no_data` appelle simplement
`send_usb_control_request`, qui est `DeviceRequestTO` sur le device
user client. libopenra1n utilise donc exactement le même transport
que nous ; l'interface user client ne sert QUE pour l'upload pongo
post-exploit. Reste donc, comme seule différence, la forme de
l'overwrite (0x00/0x00) — déjà testée en E11/E12.

### E16 — Exécuter l'implémentation de RÉFÉRENCE (gaster de 0x7ff)

**Question** : est-ce NOTRE port qui est en cause, ou bien ce Mac +
ce câble + A9 ne peuvent-ils pas être pwn par checkm8 tout court ?

**Test** : `git clone https://github.com/0x7ff/gaster` (auteur de
palera1n, source du `gaster_ref.c` étudié depuis le début) dans
`~/Documents/git/github/maxime1907/gaster`, compilé sur place
(`make` → binaire de 71 800 octets, 14 warnings bénins), puis
`./gaster pwn` en pseudo-terminal (sa boucle `while(stage !=
STAGE_PWNED)` ne sort jamais seule ; sortie bufferisée sinon).

**Résultat** : **SUCCÈS — `PWND:[checkm8]` obtenu** sur ce Mac M-series
+ câble USB-C→Lightning **direct**, sans hub, sans replug.

```
Stage: RESET  ret: true
Stage: SETUP  ret: true
Stage: SPRAY  ret: true
Stage: PATCH  ret: true
Now you can boot untrusted images.
USB Serial Number = "CPID:8000 ... SRTG:[iBoot-2234.0.0.3.3] PWND:[checkm8]"
```

**Verdict** : checkm8 EST possible sur cette plateforme → notre port a
un défaut précis. Le journal passe en mode « diff contre le binaire
gagnant ».

### E16bis — Diff structurel notre stage 3/4 ↔ gaster gagnant

Lecture complète de `~/Documents/git/github/maxime1907/gaster/gaster.c`
(clone 0x7ff du jour, la version qui vient de réussir) + dump de sa
config A9 via instrumentation (`GASTER_DUMP=/tmp/gaster-e16.txt`) :

**Identique à nous (vérifié) :**
- table 0x8000 : toutes les adresses identiques à notre `chip_db`
  (patch_addr 0x10000812C, memcpy 0x100011030, gUSBSerialNumber
  0x180087958, dfu_handle_request 0x1800878F8, usb_core_do_transfer
  0x10000EE78, dfu_handle_bus_reset 0x180087928,
  handle_interface_request 0x10000F1B0, usb_create_string_descriptor
  0x10000E354, usb_serial_number_string_descriptor 0x1800807DA,
  insecure_memory_base 0x180380000, boot_tramp_end 0x1800E1000,
  config_overwrite_pad 0x500) ;
- payload : `data_sz = (280−104) + 104 + (248−56) + 56 = 528` — layout
  identique au nôtre (main 176 + tail 104 + handler 192 + handler_tail
  56), **pas de ROP pour A9** (tlbi=0, ttbr0_sram_off=0), champs
  remplis dans le même ordre, même tag ` PWND:[checkm8]`. Notre
  `assemble_payload` est logiquement identique.
- spray A9 : `stall (0x02,0x03,0,0x80,0)` + `leak (GET_DESCRIPTOR 64)`
  + `no_leak (193)` + `CLR_STATUS (193 zéros, sync, résultat ignoré)`.

**Différences structurelles (candidates au défaut) :**

1. **Séparation stricte des stages** : après CHAQUE stage, gaster fait
   `ResetDevice(handle->device)` + `USBDeviceReEnumerate(handle->device, 0)`
   + fermeture du handle, puis rouvre un handle **neuf** pour le stage
   suivant (boucle `wait_usb_handle`). Ligne 1268/1273. Nous : stages
   3 et 4 sur le même handle (ré-activé seulement après wedge).
2. **Forme de l'overwrite : `(0x02, 0x03, 0, 0x80, 48)`** — la forme
   SET_FEATURE(ENDPOINT_HALT) AVEC 48 octets de data, et il EXIGE le
   STALL (`transfer_ret.ret == USB_TRANSFER_STALL`), puis enchaîne le
   payload **sur le même handle, fire-and-forget** (le résultat des
   DNLOAD n'est pas vérifié), puis suffix 16, zero-len 0, GETSTATUS ×3
   (résultats ignorés), puis ResetDevice+ReEnumerate.
   ⇒ Notre « règle M-series n°3 » (« SET_FEATURE pose le halt hôte →
   utiliser 0x00/0x00 ») est **FALSIFIÉE par E16** : gaster utilise
   0x02/0x03 avec succès sur cette machine.
3. **Le ResetDevice+ReEnumerate APRÈS le GETSTATUS walk** déclenche
   `dfu_handle_bus_reset` → exécution du payload → PWND dans le serial.
   Notre E11/E12 faisait aussi ResetDevice+ReEnumerate final, mais avec
   un overwrite 0x00/0x00 qui n'a probablement jamais atterri (H1 :
   le device DFU normal accepte le payload dans son buffer de download
   → walk MANIFEST propre = faux succès).

**Hypothèse unifiée post-E16** : l'overwrite doit être 0x02/0x03 (la
requête atteint le handler EP0 du ROM et sa data phase de 48 octets
atterrit dans le io_buffer libéré ; le 0x00/0x00 est rejeté au SETUP
**sans data phase** → rien n'atterrit), et le device doit être remis
à zéro (ResetDevice+ReEnumerate) entre spray et patch — le STALL de
l'overwrite et le payload ne passent que sur ce device fraîchement
resetté.

**E17 (protocole)** : instrumenter gaster pour tracer CHAQUE transfert
(bmRequestType, bRequest, wValue, wIndex, wLength, IOReturn, wLenDone,
bornes de stage), remettre le device en DFU normal, relancer `pwn`,
capturer la séquence gagnante exacte, puis la porter verbatim dans
`iokit_usb.c`/`checkm8_*` (E18).

### E18 — Portage de la séquence gagnante (diff E16bis appliqué)

**Changements dans notre code** (un seul run de validation ensuite) :

1. `checkm8_patch.c` : overwrite `(0x00,0x00,0,0x00,48)` →
   **`(0x02,0x03,0,0x80,48)`** (gaster exact, STALL requis) — ARMv7 et
   AArch64 ;
2. `checkm8_patch.c` : **suppression du rebuild E14** entre overwrite
   et payload (gaster enchaîne sur le MÊME handle, gaster.c L1211-1216) ;
3. `checkm8.c` : **reset inter-stage** spray→patch
   (ResetDevice + ReEnumerate + deactivate + 250 ms + activate — la
   séparation stricte des stages de gaster, L1268/1273) ;
4. `checkm8_patch.c` : diagnostic E15 (GETSTATUS pré-overwrite)
   **désactivé par défaut**, réactivable par `TR4MPASS_PRE_STATUS=1` —
   le run gagnant n'envoie AUCUNE requête entre la fin du spray et
   l'overwrite, et une requête en plus est une divergence.

**Divergence mineure assumée** : `iokit_usb_activate()` envoie un
`AbortPipeZero` à l'ouverture du handle ; gaster ne le fait pas
(l'abort n'existe que dans son chemin async). Sur un handle frais,
rien n'est en vol → no-op. Conservé.

**Timeouts E18** : tout le stage 4 sync passe à **5 ms** (gaster
`usb_timeout` — la config gagnante), overriding possible par
`TR4MPASS_STAGE4_TIMEOUT_MS=5000` pour reproduire le comportement
E11/E12. Avant ce changement, notre payload/suffix/GETSTATUS
utilisaient `USB_TIMEOUT_MS=5000` — 4 divergences de plus vs gaster.

Le CLR_STATUS stage 3 reste ACTIVÉ (gaster l'envoie, 193 zéros, sync,
résultat ignoré — cf. E13 pour le comportement wedge, qui est aussi
celui de gaster sur cette machine puisque son run gagne).

Build clean, 62/62. Run de validation quand le device est en DFU
normal.

**Résultat E18** (`/tmp/tr4mpass-e18.log`) : **PROGRÈS MAJEUR —
l'overwrite a atterri pour de vrai, preuve par crash.**

- overwrite `0x02/0x03` STALL propre (wLenDone=0) ;
- payload/suffix/zero-len/GETSTATUS : `-7` ×6, `wLenDone=0` partout
  → **la data phase du payload n'est JAMAIS partie** (pas un STALL :
  un timeout) ;
- **mais le device a QUITTÉ le DFU et rebooté sous iOS** (serial UDID,
  jamais vu depuis E5b où il restait proprement en DFU). Interprétation :
  le callback hijacké (0x180380000) a **exécuté** au reset bus — mais
  le buffer à cette adresse ne contenait pas notre payload (data
  phase jamais transférée) → déchets du spray → exception → reboot.
  C'est la première fois que le callback S'EXÉCUTE réellement :
  l'arme est armée, il ne manque que l'atterrissage des 528 octets.

Vérifications passées en même temps : nos blobs embarqués
(`payload_A9_bin` 280, `payload_handle_checkm8_request_bin` 248) sont
**byte-exact identiques** aux bins upstream 2026 qui ont gagné → le
payload n'est pas en cause. Reste donc : la mécanique qui fait passer
le DNLOAD 528 après le STALL d'overwrite chez gaster.

**E17 = LA mesure qui tranche** : trace requête-par-requête du gaster
gagnant (`GASTER_TRACE`, instrumentation compilée dans le binaire —
SYNC/ASYNC_SUBMIT/ASYNC_DONE + bornes de stage). Comparaison
ligne-à-ligne contre notre log E18 : chaque divergence (stage 1
reset : gaster fait un cycle DNLOAD16+MANIFEST+DNLOAD64 complet,
nous juste ABORT/CLRSTATUS ; spray ; CLR ; overwrite→payload) sera
visible en noir sur blanc.

### E17 — RÉSULTAT : trace complète du run gagnant (device en DFU normal)

Run exécuté par l'utilisateur, device en DFU normal, gaster
instrumenté. **PWND obtenu** (« Now you can boot untrusted images. »).
Trace brute verbatim (`/tmp/gaster-trace.log`, format :
`SYNC|ASYNC_SUBMIT|ASYNC_DONE bmReq bReq wValue wIndex len= ret= sz=`,
IOReturn en hex, bornes de stage) :

```
SYNC 80 06 0100 0000 len=18 ret=0x0 sz=18
SYNC 80 06 0304 0409 len=255 ret=0x0 sz=196
=== STAGE RESET ===
SYNC 21 01 0000 0000 len=16 ret=0x0 sz=16
SYNC 21 01 0000 0000 len=0 ret=0x0 sz=0
SYNC a1 03 0000 0000 len=6 ret=0x0 sz=6
SYNC a1 03 0000 0000 len=6 ret=0x0 sz=6
SYNC a1 03 0000 0000 len=6 ret=0x0 sz=6
SYNC 21 01 0000 0000 len=64 ret=0x0 sz=64
SYNC 80 06 0100 0000 len=18 ret=0x0 sz=18
SYNC 80 06 0304 0409 len=255 ret=0x0 sz=196
=== STAGE SETUP ===
ASYNC_SUBMIT 21 01 0000 0000 len=2048 abort=4
ASYNC_DONE 21 01 0000 0000 len=2048 sz=2048 status=0
SYNC 21 01 0000 0000 len=64 ret=0x0 sz=64
ASYNC_SUBMIT 21 01 0000 0000 len=2048 abort=5
ASYNC_DONE 21 01 0000 0000 len=2048 sz=2048 status=0
SYNC 21 01 0000 0000 len=64 ret=0x0 sz=64
ASYNC_SUBMIT 21 01 0000 0000 len=2048 abort=0
ASYNC_DONE 21 01 0000 0000 len=2048 sz=64 status=1
SYNC 00 00 0000 0000 len=1216 ret=0xe000404f sz=0
SYNC 80 06 0100 0000 len=18 ret=0x0 sz=18
SYNC 80 06 0304 0409 len=255 ret=0x0 sz=196
=== STAGE SPRAY ===
SYNC 02 03 0000 0080 len=0 ret=0xe000404f sz=0
ASYNC_SUBMIT 80 06 0304 000a len=64 abort=1
ASYNC_DONE 80 06 0304 000a len=64 sz=0 status=1
ASYNC_SUBMIT 80 06 0304 000a len=193 abort=1
ASYNC_DONE 80 06 0304 000a len=193 sz=0 status=1
SYNC 21 04 0000 0000 len=193 ret=0xe00002ed sz=0
SYNC 80 06 0100 0000 len=18 ret=0x0 sz=18
SYNC 80 06 0304 0409 len=255 ret=0x0 sz=196
=== STAGE PATCH ===
SYNC 02 03 0000 0080 len=48 ret=0xe000404f sz=0
SYNC 21 01 0000 0000 len=528 ret=0xe0004051 sz=0
SYNC 21 01 0000 0000 len=16 ret=0xe0004051 sz=0
SYNC 21 01 0000 0000 len=0 ret=0xe0004051 sz=0
SYNC a1 03 0000 0000 len=6 ret=0xe0004051 sz=0
SYNC a1 03 0000 0000 len=6 ret=0xe0004051 sz=0
SYNC a1 03 0000 0000 len=6 ret=0xe0004051 sz=0
SYNC 80 06 0100 0000 len=18 ret=0x0 sz=18
SYNC 80 06 0306 0409 len=255 ret=0x0 sz=226
```

(Sans bornes de stage : lecture du serial à chaque `wait_usb_handle` —
18 B descriptor + 196 B serial normal entre chaque stage ; le dernier
`sz=226` = **196+30 = PWND:[checkm8] ajouté**.)

### E17-premières — Premières pensées (à valider par les sous-agents)

**P1 — LE payload DNLOAD gagnant ÉCHOUE AUSSI chez gaster** :
`SYNC 21 01 len=528 ret=0xe0004051 sz=0` — timeout IOKit, 0 octets
reportés, **exactement notre E18** (`-7` = 0xe0004051 mappé). suffix
16, zero-len, GETSTATUS ×3 : `0xe0004051` chez lui comme chez nous.
⇒ le résultat USB du POST-overwrite n'a **aucune importance** : chez
lui tout « échoue » au niveau IOKit et le PWND apparaît quand même.
Conséquence directe : la data phase des 528 o est **DMAée côté device
AVANT le timeout du STATUS** (ou ne l'est pas du tout — le PWND vient
d'ailleurs) ; `wLenDone/sz=0` au timeout est un **artefact hôte
non fiable** (E11b). Notre lecture « payload jamais parti » (E18) est
probablement **fausse**.

**P2 — Puisque le post-overwrite est identique, la divergence est
AVANT le PATCH.** Wire-diff E17↔E18, stage par stage :

| Point | gaster gagnant | notre E18 |
|---|---|---|
| STAGE RESET | cycle complet : DNLOAD 16 OK, zero-len OK, **3× GETSTATUS OK** (walk MANIFEST), DNLOAD 64 OK | GETSTATUS seul (state 0x02) + dfu_reset_to_idle no-op — **JAMAIS de cycle** |
| SETUP attempt 1-2 | async 2048 **complet** (sz=2048) + DNLOAD 64 recovery | idem (sz=2048, recovery 64) — identique |
| SETUP attempt 3 (abort=0) | async 2048 : **device STALL après 64 o acceptés** (`sz=64 status=1`) — 64 o DANS le buffer | cancel libusb à 0 ms : **sz=0, RIEN parti** (status=3 cancelled) |
| padding | `(0,0,0,0,1216)` **STALL** (`0xe000404f`) | `(0,0,0,0,1280)` timeout-as-stall (`-7`) **taille différente** (1216 vs 1280 !) |
| SPRAY | identique (stall/leak/no_leak/CLR wedge 0xe00002ed — **même IOReturn que nous**) | identique |
| PATCH | overwrite 48 STALL puis tout `0xe0004051` | identique |
| après final reset | device REVENUE en DFU, serial **sz=226 = PWND** | device **partie sous iOS** (reboot) |

**P3 — Deux différences wire réelles avant le PATCH** :
(a) stage 1 : gaster force le cycle DNLOAD16+walk+DNLOAD64 (état DFU
déterministe : le device traverse MANIFEST et repart propre) ;
(b) SETUP attempt 3 : chez gaster, 64 o atterrissent device-side dans
le buffer AVANT le STALL (`sz=64 status=1`) ; chez nous le cancel
libusb part avant tout octet (`sz=0`) ⇒ **notre buffer libéré est
vide, le sien contient 64 o**. Le padding diffère en conséquence
(1216 = 0x500−64 chez lui ; 1280 chez nous).

**P4 — Le reboot-sous-iOS de E18 se réexplique** : si la data phase du
DNLOAD 528 atterrit quand même côté device (P1), alors chez NOUS elle
est peut-être arrivée à un **mauvais endroit** (buffer vide au mauvais
offset, chaîne io_request différente à cause de (b)) → payload exécuté
à une adresse décalée → crash → reboot iOS. Chez gaster, tout tomber
pile (grâce à (a)+(b)) → PWND.

**P5 — Question ouverte pour les sous-agents** : le DNLOAD 528
post-STALL (timeout côté hôte) transfère-t-il réellement sa data phase
côté device ? Si OUI, P4 explique tout et la correction = répliquer
(a)+(b) exactement. Si NON, le PWND vient d'un autre chemin (à
identifier). Test discriminator possible sans device : log `wLenDone`
via un transfert de contrôle OUT volontairement timeouté vers un hub
quelconque et observer si IOKit reporte des octets (fiabilité du
champ au timeout).

**P6 — Corrections candidates E19 (ordre gaster strict)** :
1. stage 1 = cycle gaster complet (DNLOAD16 → zero-len → 3×GETSTATUS
   MANIFEST walk → DNLOAD64) au lieu de GETSTATUS+reset_to_idle ;
2. stage 2 attempt 3 : le cancel libusb à abort=0 doit laisser passer
   le premier paquet 64 o comme chez gaster (essayer abort=1 ms ? ou
   soumettre puis AbortPipeZero immédiat côté IOKit — gaster utilise
   DeviceRequestAsyncTO+sleep(0)+USBDeviceAbortPipeZero, NOUS libusb
   cancel) ;
3. padding = `0x500 − sz` où sz = bytes réellement partis (64 chez
   gaster, 0 chez nous) — découle de (2) ;
4. vérifier que notre inter-stage reset entre spray et patch ne
   diffère pas du sien (ResetDevice+ReEnumerate, close, reopen).

### E19 — Portage complet des verdicts (P1–P6 appliqués)

**Verdicts sous-agents** (2/3 ont livré ; le 3e, audit lecture-seule,
a refusé — refait à la main, findings ci-dessous) :

- **P5 = H1, confiance ~95%** (`/tmp/sb-p5-verdict.md`, 17 sources) :
  sur IOKit `DeviceRequestTO`, un timeout hôte `wLenDone=0` **ne dit
  rien des octets déjà ACKés device-side** — la data phase d'un DNLOAD
  timeouté peut être intégralement livrée. Preuve empirique interne :
  le run gagnant rapporte `ret=0xe0004051 sz=0` sur le payload 528
  **et le PWND apparaît** — les 528 o n'existent que dans cette data
  phase. Même chose pour le STALL d'overwrite (48 o) : sz=0 ET hijack
  confirmé. ⇒ **notre conclusion E18 « payload jamais parti » était
  fausse** : le payload est probablement arrivé, mais dans un mauvais
  état de buffer (P4 confirmé).
- **P3 portage** (`checkm8_stages.c`, 177+/54−) : (1) stage 1 =
  cycle gaster exact DNLOAD16 → zero-len → 3×GETSTATUS (MANIFEST
  walk vérifié par (status,state)) → DNLOAD64, tout en
  `DeviceRequestTO` 5 ms, échec = CLRSTATUS + fail-fast ; (2) stage 2
  attempt 3 = `DeviceRequestAsyncTO` + abort immédiat via
  `iokit_usb_ctrl_transfer_async_abort` (même infra que
  request_stall/leak — port direct du `send_usb_control_request_async`
  IOKit de gaster) → cible `sent=64` + STALL ; (3) padding =
  `0x500 − sz_réel` (1216 si 64). Build 0 warning, 62/62.

**Audit orchestration (refait à la main) — 4 divergences trouvées,
toutes corrigées** (`checkm8.c`) :

1. **Le run gagnant est IOKit de bout en bout** : `make macos` de
   gaster compile SANS `-DHAVE_LIBUSB` → les 4 stages tournent sur
   `DeviceRequestTO`/`AsyncTO` (les IOReturns bruts `0xe000404f`/
   `0xe0004051` de la trace le prouvent). Nous : stages 1-2 en
   libusb → attempt 3 prenait le cancel libusb (`sent=0`, la
   divergence P3b complète). **Corrigé : IOKit activé AVANT le stage 1**
   (`usb_ctrl_transfer_raw` route déjà vers IOKit quand actif) ;
   garde `checkm8_stage_reset` assoupli (handle libusb NULL autorisé
   si IOKit actif).
2. **Reset inter-stage manquant reset→setup** : gaster reset après
   CHAQUE stage (L1347 dans la boucle). Nous : seulement spray→patch.
   Le nouveau cycle stage 1 finit par un DNLOAD 64 chargé → le stage 2
   devait démarrer sur un handle frais. **Corrigé : reset après chaque
   stage** (helper `dfu_iokit_interstage_reset` : ResetDevice +
   ReEnumerate + deactivate + 250 ms + activate), aux 3 frontières.
3. **Reset FINAL manquant = le trigger d'exécution** : après le
   dernier stage, gaster fait reset_usb_handle **inconditionnellement**
   avant close (L1347→1352) — c'est CE ResetDevice+ReEnumerate qui
   déclenche le callback `dfu_handle_bus_reset` hijacké, i.e. **qui
   exécute le payload**. Nous : simple `deactivate()` sans reset.
   **Corrigé : reset final avant deactivate**, puis `checkm8_verify`
   (qui gère déjà le chemin IOKit) relit le serial re-énuméré → PWND.
4. **Vérifié sans divergence** : spray A9 (stall/leak 64/leak 193/
   no_leak 193/CLRSTATUS len=193, IOReturns identiques à la trace),
   overwrite 0x02/0x03+48 o STALL, payload 528+suffix 16+zero-len+
   GETSTATUS×3 en 5 ms results-ignored, E15 derrière
   `TR4MPASS_PRE_STATUS` (défaut OFF). Détails gaster notés : sa
   boucle spray reloooped stall/leak/no_leak jusqu'au succès et il
   exige le STALL de l'overwrite (sinon échec → retry complet) — nos
   chemins loggent et continuent, acceptable pour l'instant.

**Build/tests** : 0 warning, 62/62.

**Run de validation (à faire par l'utilisateur)** — device en DFU
normal (il a rebooté sous iOS en E18) :

```sh
cd /Users/mleroy/Documents/git/github/maxime1907/tr4mpass && ./tr4mpass -v 2>&1 | tee /tmp/tr4mpass-e19.log
ioreg -p IOUSB -l -w0 | grep -i "USB Serial Number"
```

Attendu gaster-exact : stage 1 = 6 requêtes SYNC toutes `ret=0`
(16/0/6/6/6/64) ; setup attempt 3 = `sent=64` + STALL ; padding
**1216** STALL (`0xe000404f`) ; spray identique E18 ; overwrite 48
STALL ; payload/suffix/zero-len/GETSTATUS `0xe0004051` (ignorés) ;
**reset final** ; serial `sz=226` avec `PWND:[checkm8]`.

### E19 — RÉSULTAT : **PWND:[checkm8] OBTENU** ✅

Run utilisateur, device en DFU normal, câble direct USB-C→Lightning,
sans hub, sans replug. Log `/tmp/tr4mpass-e19.log`. Preuve :

```
"USB Serial Number" = "CPID:8000 CPRV:20 CPFM:03 SCEP:01 BDID:02
ECID:000001756F4E751C IBFL:1C SRTG:[iBoot-2234.0.0.3.3] PWND:[checkm8]"
```

`checkm8_verify: device processed successfully` (deux lectures
cohérentes). Le device est resté en DFU pwned (pas de reboot).

**Séquence réellement gagnante** (vs prédictions E19) :

| Étape | Prédit (E19) | Réel (E19) |
|---|---|---|
| transport | IOKit dès stage 1 | ✔ `IOKit active for ALL stages` |
| stage 1 | cycle gaster complet | ✔ cycle exécuté, stage 1 OK |
| resets inter-stage | ×3 + final | ✔ ×3 + final (jamais de quit-DFU) |
| attempt 3 | `sent=64` + STALL | **`sent=0`** (artefact async, cf. dessous) |
| padding | 1216 | **1280** — mais **STALL (-9)** ✔ |
| overwrite 0x02/0x03+48 | STALL | ✔ STALL (`-9`, "UAF write landed") |
| payload 528/suffix/zero-len/GETSTATUS | `0xe0004051` ignorés | ✔ `-7` ignorés |
| reset final → serial | PWND | ✔ **PWND:[checkm8]** |

**Lecture scientifique de l'écart attempt 3** : le chemin async IOKit a
renvoyé `sent=0` (le compte hôte n'a pas capté les octets partis avant
l'abort — même famille d'artefact que P5/H1 : ce que rapporte l'hôte au
moment d'un abort n'est pas ce que le device a ACKé). Le device a
vraisemblablement accepté le 1er paquet 64 o **côté sien**, puis le
padding 1280 a STALLé exactement comme le 1216 de gaster. Deux lectures
possibles, indiscernables dans ce run : (a) la taille du padding a une
tolérance large (le rôle du pad est de consommer le trou jusqu'au
STALL, la frontière est déterminée device-side) ; (b) les octets
réellement ACKés diffèrent du compte hôte et l'alignement s'est fait
quand même. **Ce qui est établi** : la recette gagnante complète =
IOKit de bout en bout + cycle stage 1 gaster + reset inter-stage à
CHAQUE frontière + reset final (trigger d'exécution) + overwrite
0x02/0x03+48 + résultats post-overwrite ignorés. Les tailles padding
exactes (1216 vs 1280) et le compte attempt 3 ne sont **pas** des
conditions critiques sur cette machine.

**Verdict global** : objectif du doc atteint — checkm8 fonctionne sur
Apple Silicon M-series, câble direct, sans hub, sans replug, avec le
port tr4mpass (payloads byte-exact gaster). E0→E19 clos.

### E20 — Transition post-exploit (étape 3 du pipeline, phase préparatoire)

**Prochaine frontière** : après checkm8, `path_a` exécute 6 étapes de
plus ([3/8] ramdisk → [8/8] vérification). L'étape [3/8] charge iBSS →
iBEC → devicetree → ramdisk via `libirecovery` avec **3 reconnexions
USB** (DFU 0x1227 → recovery 0x1281 → iBEC) — exactement la zone que
palera1n documente comme fragle sur M-series USB-C (« may require
manual unplugging and replugging **after** checkm8 »). Problème
distinct du E19, traité séparément.

**Outils construits** (dans `scripts/`, réutilisables sans agent) :

- `scripts/ipsw_pull.py` — extraction d'UN fichier d'un IPSW distant
  via HTTP Range (lecture du central directory ZIP en fin de fichier,
  téléchargement de la seule entrée visée : ~300 Ko au lieu de 2,2 Go).
  Sous-commandes : `list`, `url-for <device> <build>`, `probe`
  (extrait + cherche une aiguille, ex. `iBoot-`), `fetch`.
- `scripts/detect_ios.sh` — attend le device en mode NORMAL et écrit
  version/build/modèle dans /tmp/ios_{version,build,model}.txt.
- `scripts/ramdisk_test_ibss.sh` — test A complet : détection iOS →
  pull iBSS → upload sur DFU pwned → surveillance PID 0x1227→0x1281.
  Ne touche jamais au stage checkm8 (pré-suppose PWND).

**Découverte SRTG (correction de croyance)** : `SRTG:[iBoot-2234.0.0.3.3]`
est la version du **BootROM** (SecureROM Tag) de l'A9, PAS celle
d'iOS — identique sur tous les iPhone 6s. Vérifié empiriquement en
extrayant les iBSS de 4 IPSW (9.0 → `iBoot-2817.1.93`, 9.0.2 →
`2817.1.94`, 9.1 → `2817.10.34`, 9.3.5 → `2817.60.2`) : jamais 2234.
⇒ le serial DFU ne permet pas de déduire la version iOS ; détection
via `ideviceinfo` (mode normal) nécessaire. Note ajoutée en tête du doc.

**Fait saillant iPhone 6s** : l'IPSW contient DEUX iBSS — `iBSS.n71`
(A9 TSMC) et `iBSS.n71m` (A9 Samsung, double-sourcing). Le BDID du
device choisit ; le script essaie les deux.

**Prochain run** (à faire par l'utilisateur) : device actuellement en
DFU pwned.

```sh
# Passe 1 (consomme le pwned) : le device boote iOS, on lit la version
scripts/ramdisk_test_ibss.sh
# ... détection, téléchargement iBSS, puis "device left pwned DFU"

# Remise en DFU + re-pwn (reproductible, cf. E19) :
#   boutons DFU puis ./tr4mpass -v

# Passe 2 (détection cachée) : upload iBSS + surveillance PID
scripts/ramdisk_test_ibss.sh
```

Critère de succès : `PASS` final (PID 0x1281 observé sans replug). En
cas d'échec, on saura que la transition USB-C post-pwn est le vrai
sujet M-series et on travaillera le reconnect (hub recommandé par
palera1n, ou gestion IOKit custom dans path_a_ramdisk).

| **E20** | **?** | phase préparation étape [3/8] : outils ipsw_pull/detect_ios/ramdisk_test + découverte SRTG≠iOS | outils testés (url-for OK, probe OK sur 4 builds) — **run iBSS en attente** | run |

| Source | Ce qu'elle établit |
|---|---|
| DSEC/Habr, « Technical analysis of the checkm8 exploit » (2019) — https://habr.com/en/companies/dsec/articles/472762/ | Mécanisme complet : heap feng-shui, UAF, overwrite à 0x5c0 (T8010), callback-chain via gadget `[X0+0x70]`, shellcode. Point clé transport : « le device continue de recevoir tous les SETUP suivants et les place dans sa queue » → après un STALL, **le device est sain**. PoC Arduino : le SETUP+IN annulé en cours de transaction suffit — tout le reste est du traffic normal. |
| ipwndfu `checkm8.py` (axi0mX) — https://github.com/axi0mX/ipwndfu | Flow original intégral. Après l'overwrite (`libusb1_no_error_ctrl_transfer(device, 0, 9, 0, 0, overwrite, 100)` — SET_CONFIG !) : payload en DNLOAD 0x800, suffix, zero-len, GETSTATUS ×3, `usb_reset` — **aucun résultat vérifié, aucun abort, aucune sonde**. |
| palera1n README — https://github.com/palera1n/palera1n | « USB-C port on Apple Silicon Macs may require manual unplugging and replugging of the lightning cable **after** checkm8 exploit » → le souci M-series documenté est l'étape APRÈS l'exploit, pas l'exploit. |
| checkra1n, « The state of checkra1n on Apple Silicon Macs » (04/2021) — https://checkra.in/news/2021/04/M1-announcement | Le bug USB stack M-series (crash device) ne touche que **A7, A9X, A10, A10X** — l'A9 passe. → un transport checkra1n fonctionnel existe pour A9 sur USB-C M-series. |
| palera1n-c `src/usb_iokit.c` (dérivé pongoOS, archivé) — https://github.com/palera1n/palera1n-c | Le transport IOKit de production checkra1n : `(*handle)->ControlRequest(handle, 0, &request)` sur `IOUSBInterfaceInterface` (interface user client, pipe 0), `IOUSBDevRequest` **sans timeout**, ouvert via `USBInterfaceOpen`. Diffère du device user client `DeviceRequestTO` de gaster. |
| gaster (0x7ff), IOKit backend re-lu | `send_usb_control_request` = `DeviceRequestTO` 5 ms, retour always-true, résultat du payload ignoré ; async+abort uniquement pour stall/leak/no_leak. C'est le chemin device user client. |

**Lecture d'ensemble** : tout le monde s'accorde sur le protocole device
(spray → CLR → overwrite STALL → payload → suffix → zero-len → GETSTATUS ×3
→ reset). La seule différence structurante entre « outils qui marchent sur
M-series » (checkra1n/pongoOS) et « notre chemin qui wedge » est le **user
client IOKit utilisé** : interface (ControlRequest, sans timeout) vs device
(DeviceRequestTO, TO). C'est l'objet de l'expérimentation E9.

---

## 13. E20 → E23 — Modèle réel du device, no-boot iBSS, et canal exec

### E20 — Le device n'est PAS un iPhone 6s

- Serial DFU : `CPID:8000 BDID:02` → table libirecovery L195-196 :
  `{ "iPhone8,4", "n69uap", 0x02, 0x8000, "iPhone SE (1st gen)" }`.
  Le « iPhone 6s » de la doc de recherche était faux (n71 = BDID 0x04).
- Vérifié live après boot iOS : `ideviceinfo` → ProductType **iPhone8,4**,
  HardwareModel **N69uAP**, **iOS 14.6 / build 18F72**.
- Conséquence : les images n71 précédemment fetchées étaient du mauvais
  board → explique le no-boot silencieux #1. Images re-fetchées pour
  18F72 (iBSS/iBEC/DeviceTree n69u + ramdisk 018-18125-090.dmg Erase
  + trustcache), env.sh réécrit. Artifacts n71 supprimés.

### E21 — `irecovery -f` ne boote PAS un payload sur DFU pwned gaster

- iBSS.n69u (mauvais board d'abord, puis bon board) uploadé à 100 %,
  rc=0, 3 fois. Aucune transition DFU→recovery.
- Vérifié dans libirecovery.c (source upstream) : `-f` = DNLOAD 0x800
  chunks → ZLP (wValue=packets) → 2× GETSTATUS → `irecv_reset`.
  C'est la séquence **stock DFU** ; sur le runtime pwned gaster, le
  hook `dfu_handle_request` n'interprète pas cette danse comme un
  ordre de boot (pas de suffix 16 octets, mauvaise marche d'états).

### E22 — Séquence de boot correcte = celle de gaster (dfu_boot)

Outil `tools/dfu_boot.c` reproduisant gaster `dfu_send_data` +
`dfu_set_state_wait_reset` : DNLOAD chunks 0x800 → suffix 16 zéros →
ZLP → GETSTATUS ×3 (états 6→7→8, observés **exactement**) →
`irecv_reset`. La marche d'états réussit (le hook DFU tourne) mais le
device re-rentre en pwned DFU au reset : **l'image uploadée n'a jamais
été exécutée**. Deux causes identifiées ensuite (E23) :

1. Le trigger `0x21/4` de libopenra1n (palera1n) est un hook de
   **leur** payload, pas de gaster — envoyé, ignoré (r=-0, no-op).
2. L'image envoyée était le .im4p **chiffré GID** : même si le hook
   l'avait copié et sauté dessus, c'est du garbage chiffré. Le SRTG
   iBoot-2234.0.0.3.3 réapparaît au reset = payload intact qui
   ré-arme pwned DFU (comportement normal).

### E23 — Canal exec : transport OK, résultat toujours vide

- `decrypt_kbag` de gaster AUTHENTIQUE (compilé depuis master
  0x7ff/gaster, payloads byte-identiques aux nôtres — vérifié par
  extraction/comparaison des deux binaires embarqués) échoue pareil :
  GET 0xA1/2 wValue=0xFFFF → timeout (libirecovery `-11`) ou
  `0xe0004051` (notre transport E9 IOKit interface).
- Séquence exacte de gaster reproduite (DNLOAD data → suffix 16 → ZLP
  → états 6→7→8 → GET sur le MÊME handle, PAS de reset) : identique.
- GETSTATUS/plain GET 0xA1/2 wValue=0 : timeout aussi — or le hook
  `_request_done` répond à TOUT 0xA1/2 (pass-through 0x80 octets).
  ⇒ Symptôme : la branche `handle_interface_request` ne s'exécute
  JAMAIS, ou `insecure_memory_base` ne contient pas ce qu'on croit.
- Piste principale : notre E19 a installé le payload avec la recette
  stage-4 modifiée (overwrite 0x02/0x03+48B, resets intermédiaires ×3).
  Le hook DFU (chaîne `dfu_handle_request` → `_main` →
  `handle_interface_request`) fonctionne pour les requêtes DFU
  standard, mais si le **tail** du handler (exec_magic, done_magic,
  insecure_memory_base) est corrompu ou pointe ailleurs, la branche
  exec (0x2A1 / 0xFFFF) ne matche jamais → timeout. À noter : la
  lecture PWND: fonctionne (string descriptor), la marche d'états
  aussi — tout ce qui passe par le début du hook marche ; tout ce qui
  dépend du tail ne répond pas.
- Test discriminant programmé (E24) : DFU vierge → `gaster pwn`
  STOCK (aucune modification) → `decrypt_kbag` immédiatement. Si OK,
  notre recette stage-4 est en cause (revenir à l'overwrite gaster
  0x00/0x00 exact, sans resets intermédiaires supplémentaires) ;
  si KO, le problème est transport/plateforme (IOUSBHost = plan B).

### Journal

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E20** | **🛑** | device = iPhone8,4/n69u (BDID 0x02 + CPID 0x8000), PAS 6s/n71 ; iOS réel 14.6/18F72 | images n71 jetées, re-fetch 18F72 correct | boot gate avec iBSS.n69u déchiffré |
| **E21** | **🛑** | `irecovery -f` (séquence stock) ne boote rien sur pwned DFU gaster (3 uploads 100 %, 0 transition) | outil dédié dfu_boot requis | sursédé |
| **E22** | **🛑** | séquence gaster exacte (suffix+états 6→7→8+reset) : états OK, image jamais exécutée ; trigger 0x21/4 = hook palera1n, no-op sur gaster | cause = image chiffrée + exécution jamais déclenchée | déchiffrement GID requis |
| **E23** | **🛑** | canal exec muet y compris avec gaster stock sur le pwn existant ; pass-through GET 0xA1/2 wValue=0 timeout aussi | hook DFU partiellement fonctionnel, branche exec/tail suspectée corrompue par notre recette stage-4 E19 | E24 : pwn stock → decrypt_kbag → boot |

### E24 — Discriminante gaster stock : NOTRE recette E19 corrompait le canal exec

Protocole : DFU vierge → `gaster pwn` STOCK (aucune modif) →
`decrypt_kbag` → `decrypt iBSS`.

| Étape | Résultat |
|---|---|
| gaster pwn stock (4 stages) | ✅ tous `ret: true` |
| `decrypt_kbag` (kbag 18F72 extrait du iBSS) | ✅ **IV/key déchiffrées retournées** |
| `decrypt iBSS.n69u` | ✅ binaire 239 296 o, ARM64 valide (mov x16/x17…) |

⇒ Le transport, le device et le protocole exec sont bons. C'était la
recette stage-4 modifiée de E19 (overwrite 0x02/0x03, resets ×3) qui
corrompait le **tail** du handler (`exec_magic`/`insecure_memory_base`)
— tout ce qui passe par le début du hook marchait (PWND:, états DFU),
tout ce qui dépend du tail ne répondait jamais (canal exec muet).

### E25 — boot-on-reset est DÉSACTIVÉ par design dans le payload gaster

`payload_A9.S::_main` ligne 2 : `str xzr, [dfu_handle_bus_reset]` —
gaster met à NULL le callback de bus-reset du ROM **exprès** : c'est
pour ça que PWND: survit aux resets… et c'est pour ça qu'AUCUNE
séquence « DNLOAD puis reset » ne peut booter une image après un pwn
gaster (l'image était peut-être copiée en mémoire, mais le chemin de
boot du ROM est mort). Les uploads E21/E22 n'ont donc jamais pu
booter, images déchiffrées ou non. La séquence `0x21/4` de
libopenra1n est un hook de LEUR payload (yolo), pas de gaster.

Le boot après pwn-gaster passe par un **stage2** : un deuxième payload
qui ré-énumère son propre device DFU avec un boot-on-reset vivant
(ra1npoc `common.c::payload_stage2` + `connect_to_stage2` :
DNLOAD → ZLP → 3×GETSTATUS → l'image boote, pas de hack de reset).

### E26 — ROUTE PONGOOS PROUVÉE sur M4 USB-C (fin du test A)

`ra1npoc` (kok3shidoll, payloads checkra1n dumpés v0.12.4, licence
MIT) compilé pour macOS (clang + IOKit), lancé sur DFU VIERGE :

```
checkm8_arm64: set_global_state OK, heap_occupation OK, checkmate!
sending stage2 payload → connecting to stage2 → **connected to Stage2**
sending pongoOS → BOOTED? rc=0
```

Puis le device a **booté iOS normalement** (pongoOS auto-boot du
kernel par défaut : le flag NO_AUTOBOOT n'est posé que pour S8001).
Vérifié : `ideviceinfo` → iPhone8,4, 14.6, Unactivated. Toutes les
transitions USB ont survécu (DFU→stage2→pongoOS→iOS).

**Conséquences** :
1. Le test A (iBSS route) est sans objet : la route pongoOS (E20bis
   « option pongo », 1 transition) est PROUVÉE sur cette machine.
2. Le pwn via notre code C E19 reste utile (skip-if-pwned) mais le
   boot passe par le loader ra1npoc (ou un port de sa logique stage2).
3. Prochaine étape : garder pongoOS vivant — patcher le cas S8000 de
   ra1npoc pour poser `NO_AUTOBOOT` (comme S8001), puis `pongoterm`
   pour charger KPF/ramdisk custom et `bootx` (étapes [4/8]→[5/8]).

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E24** | **✅** | gaster stock : pwn + decrypt_kbag + decrypt iBSS OK sur M4 | notre recette stage-4 E19 était en cause (tail corrompu) | réaligner stage-4 sur gaster stock si on re-pwn nous-mêmes |
| **E25** | **ℹ️** | payload gaster neutralise boot-on-reset (payload_A9.S) ; boot = via stage2 | explication définitive des no-boot E21/E22 | — |
| **E26** | **✅✅** | ra1npoc s8000 : exploit → stage2 → pongoOS → boot iOS complet sur M4 USB-C | ROUTE PONGOOS retenue ; test A obsolète | NO_AUTOBOOT + pongoterm |

### E27 — s8000 full blob : pongoOS tourne mais auto-boote le kernel

Test avec le kerninfo `pongo_shell` posé (KPF_FLAGS_PTR=0x158018,
`checkrain_option_pongo_shell`) : ra1npoc s8000 → stage2 connecté →
pongoOS envoyé → **l'écran du device affiche les messages de patching
KPF** (confirmation visuelle utilisateur) puis « recfg done /
rebooting » → device parti en recovery (PID 0x12A8, serial=UDID).

Constats :
1. **pongoOS vit** : « patched » à l'écran = xnu_hook/KPF en action.
   Le shell pongo a booté et travaillé — c'est un checkra1n pongoOS
   patché, pas le mainline (qui n'a PAS d'autoboot : `while(!gBootFlag)`
   attends indéfiniment, entry.c L210).
2. Le flag `pongo_shell` du blob n'a PAS arrêté l'autoboot : le stage2
   checkra1n s8000 construit le bloc autoboot (magic `autoboot` placé
   à 0x418e00000, mm.c L802) indépendamment des kerninfo du blob.
3. La voie s8001 de ra1npoc (main.c L386-392) est la solution prouvée
   dans le code même : `payload.pongoOS_len = 0x3F2F8` (exactement
   pongoOS.bin, 258 808 o) + `NO_AUTOBOOT` → pas de KPF/RDSK dans
   l'image → shell pongoOS vivant, piloté ensuite par pongoterm
   (rdsk → « ramdisk » → kpf → « modload » → « xargs » → « bootx »).
4. E28 = s8000 avec la recette s8001. Si le shell reste vivant, la
   fenêtre d'interaction est illimitée (pongoOS n'a pas de timeout).

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E27** | **ℹ️** | pongoOS s8000 vit (KPF « patched » à l'écran) mais stage2 auto-boote malgré pongo_shell | fenêtre trop courte via blob complet ; recette s8001 = shell vivant | E28 : truncate 0x3F2F8 + pongoterm interactif |

### E28 — SHELL PONGOOS VIVANT ET PILOTABLE (s8000, recette s8001)

ra1npoc patché (s8000 : `pongoOS_len = 0x3F2F8` + NO_AUTOBOOT, cf.
E27) sur DFU vierge :

```
checkmate! → stage2 connecté → pongoOS envoyé → BOOTED? rc=0
USB : serial "pongoOS / checkra1n 2.5.1-9234e72f", PID 0x4141
```

pongoterm (IOKit, hotplug PID 0x4141) : **[Connected]**, prompt
`pongoOS>` actif. Commandes exécutées avec succès :
- `help` → set complet : `ramdisk`, `modload`, `xargs`, `bootx`,
  `bootux`, `loadx`, `sep`, `dt`, `spawn`...
- `xargs` → « xnu boot arg cmdline: [] »
- `lsdev` → arbre des devices (system-vol, data-vol, baseband-vol...)

La fenêtre d'interaction est **illimitée** (pas de timeout, pas
d'autoboot sans bloc `autoboot`). Terminer sans bootx = le device
reste dans pongoOS ; Power+Home pour ressortir.

**Pipeline activé** : étape [4/8] (environnement d'exécution) =
pongoOS shell. Les étapes ramdisk (`ramdisk` cmd + upload bulk 0x21/1
wValue=taille puis bulk EP2, cf. pongoterm ra1npoc-mode) sont
accessibles sans replug. Le KPF sera chargé via `modload` (upload
séparé), pas incrusté dans le blob.

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E28** | **✅✅** | pongoOS shell vivant + interactif illimité (xargs/lsdev OK) | étape [4/8] PROUVÉE sur M4 USB-C | upload ramdisk 18F72 + KPF + bootx ; intégrer ra1npoc/pongoterm au repo |

### E29 — UPLOAD 105 Mo DANS LE SHELL PONGO (ramdisk 18F72 en mémoire)

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E29** | **✅✅** | Ramdisk Erase 018-18125-090.dmg (105 367 579 octets) uploadé dans `loader_xfer_recv_data` via `tools/pongo_upload`, puis consommé par la commande `ramdisk` (réponse propre, pas d'erreur) | étape [4/8] ramdisk EN MÉMOIRE — il reste bootx | bootx (iOS doit démarrer avec le ramdisk) |

Détails techniques (tous vérifiés live, device en shell 2.5.1 PID 0x4141) :

1. **Transport contrôle = interface user client** (`IOUSBInterfaceInterface245`,
   `ControlRequest` pipe 0), pas le device client. Le device client stall sur les
   requêtes 0x21 (E9 redécouvert). Le `cmd` fonctionne : `0x21/3 "<cmd>\n"`.
2. **Init d'upload** : `0x21/1 wLength=4` avec la taille en data-out
   (`reallocate_loader_xfer_data` : round-up 512, ≤128 Mo, prime le DMA EP2 OUT).
   `wLength=0` = variante 1 Mo fixe (usbloader.c:107).
3. **Stall 0xe000404f (kIOUSBPipeStalled)** : toute requête 0x21/1 stall si
   `usbloader_is_waiting_xfer==1` (DMA EP2 en attente). Récupération prouvée :
   `drain` = écrire 1 Mo de junk sur pipe 2 → DMA complété → shell de nouveau OK.
4. **Gros upload M4** : WritePipe 16 KiB atteint 99 % (104 873 984) puis bloque ;
   un drain depuis une NOUVELLE connexion complète le DMA exactement
   (104 873 984 + 493 624 = 105 367 608 = taille round-up). Fix robuste :
   `WritePipeTO` (5 s) + `ClearPipeStallBothEnds` + réouverture interface ;
   le run complet passe alors sans accroc.
5. **Padding** : la taille DMA est round-up 512 ; pad = 512 − (sz mod 512) ;
   105 367 579 → pad 485 (ma première arithmétique « 29 » était fausse).
   Upload auto-padé ; commandes `pad <n>` / `query` (lecture stdout 0xA1/1
   wLength=512) / `cmdq` (cmd + réponse déterministe) ajoutées.
6. **Diagnostic `ramdisk`** : « please upload a ramdisk » = compteur
   `loader_xfer_recv_count==0` (buffer déjà consommé ou upload non complété),
   PAS une panne du shell. Second appel après consommation → message attendu.

Outils : `tools/pongo_upload` (upload|cmd|cmdq|query|drain|pad).
Script : `scripts/pongo_load_ramdisk.sh` (cmdq déterministe).

### E30 — BOOTX AVEC KPF : LE DEVICE BOOTE, PAS DE PANIQUE

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E30** | **✅✅** | bootx avec KPF modloadé → boot iOS OK, device vivant (iPhone8,4, 14.6, Unactivated, ideviceinfo répond) | E29 paniquait car KPF absent ; avec KPF le kernel patché accepte le boot | run scripté complet reproductible ; activation ensuite |

Détails :

1. **Cause de la panique E29 confirmée** : bootx avec ramdisk mais sans KPF
   → kernel non patché → panic. Le KPF de checkra1n (Mach-O cffaedfe,
   0x15648 octets) vit à 0x3F308 dans le blob composé
   `dumpfiles/pongoOS` (1 414 476 octets) ; extraction scriptée dans
   build_ra1npoc.sh → `images/kpf_checkra1n.bin`.
2. **Run scripté complet** : ra1npoc (DFU frais) → shell →
   `pongo_load_ramdisk.sh --kpf kpf_checkra1n.bin` → upload ramdisk (105 Mo,
   pad 485) → `ramdisk` → upload KPF (87 624, pad 440) → `modload`
   (bannière KPF à l'écran, vu par l'utilisateur) → `bootx` → boot OK.
3. **Bruit USB M4** : le KPF upload a stallé à 0 (0xe0004051
   transaction timeout) puis récupéré via ClearPipeStallBothEnds ; la
   réponse `modload` a timeout côté hôte (le shell occupé) et le
   `bootx` write a eu 0xe00002ed — mais le boot est quand même passé :
   ces erreurs sont côté hôte, le device continue.
4. Le ramdisk est en RAM ; aucun changement persistant. Unattended
   reboot = retour iOS normal.

Chaîne finale prouvée : DFU frais → checkm8 (ra1npoc) → stage2 → pongoOS
shell → ramdisk 18F72 en mémoire + KPF → bootx → iOS patché vivant.

### E31 — ACTIVATION REEL : LE DEVICE EST ACTIVATION-LOCKED

| ID | Type | Constat | Décision/impact | Prochaine action |
|---|---|---|---|---|
| **E31** | **ℹ️** | ideviceactivation (libimobiledevice master, build local) → serveur Apple : « Activation Lock — linked to an Apple account s●●●●●@icloud.com » | activation normale IMPOSSIBLE sans les identifiants iCloud du compte lié ; la route tr4mpass (MAD patché + deletescript) est la seule restante SI pas d'accès au compte | décision utilisateur : identifiants disponibles ? |

Détails :

1. `ideviceactivation` absent de Homebrew ; construit depuis
   libimobiledevice/libideviceactivation master (autogen + make,
   installé dans ~/.local/opt/ideviceactivation).
2. `ideviceactivation activate` (device en mode normal, Unactivated) :
   le serveur répond Activation Lock + demande email/mot de passe en
   interactif — échec non-interactif attendu, le fond est le lock.
3. Conséquence directe : les étapes [5/8]-[6/8] de Path A
   (remplacement mobileactivationd via ramdisk) nécessitent maintenant
   un boot avec boot-args rd=md0 + deletescript — le ramdisk E30 était
   en mémoire mais non utilisé (pas de xargs rd=md0 ; caveat documenté).
