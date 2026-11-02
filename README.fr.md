# RLPlayer

[简体中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · **Français** · [Русский](README.ru.md) · [Español](README.es.md) · [العربية](README.ar.md)

**Chaque maillon, pensé pour l'écoute attentive**

Suréchantillonnage et filtrage de haute précision, chaîne audio limpide et métadonnées musicales enrichies en continu. Du traitement numérique à l'écoute quotidienne, pour faire progresser le son et l'expérience ensemble.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20%C2%B7%20macOS%2013%2B-0078D6)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)
![UI](https://img.shields.io/badge/UI%20languages-7-8A2BE2)

![RLPlayer](https://rlplayer.rainlain.com/assets/img/hero/hero-01-albums.webp)

> Windows + macOS Paquets complets bi-plateforme 7 langues d'interface GPL-3.0 Licence open source Fonctionnement hors ligne Aucune collecte de données personnelles

## Chaîne audio

`Capabilities · 01–04`

Décodage, suréchantillonnage, filtrage et modulation DSD : chaque étape peut être configurée dans les paramètres et observée en temps réel dans la chaîne de signal.

### 01 · Chaîne audio

**Visualisez chaque étape traversée par le signal**

Du fichier source au périphérique de sortie, visualisez clairement le taux d'échantillonnage, les traitements audio et l'état Bit-Perfect. La chaîne de signal vous indique si les échantillons originaux sont préservés et quels réglages modifient le son.

**Chaîne du signal (schéma)**

- **Décodage** — Flux par blocs · FLAC / DSF / WAV
- **Matrice** — Downmix multicanal · 2.0 – 7.1.4
- **Égalisation** — PEQ 20 bandes · Bypass direct disponible
- **Étalonnage** — Convolution IR · Correction acoustique de pièce
- **Suréchantillonnage** — Jusqu'à 1536 kHz · 79 filtres
- **Volume** — ReplayGain · Pass-through 0 dB
- **Dither** — TPDF / NS9 · LNS15
- **Sortie** — WASAPI exclusif · ASIO / UPnP

### 02 · Filtrage

**79 préréglages de filtres, pour un choix guidé par les mesures**

Phase linéaire, phase minimale, phase mixte et filtrage à deux étages offrent diverses méthodes de reconstruction. Consultez les réponses en fréquence et mesures archivées (bande passante, atténuation en bande atténuée, comportement transitoire) pour sélectionner le filtre adapté à vos exigences.

- Sélection distincte pour 1x et Nx (Nx peut suivre 1x)
- Phase liée au préréglage ou modifiable manuellement
- Nombre de coefficients (taps), atténuation en bande atténuée et bande de transition configurables

### 03 · Suréchantillonnage PCM

**Suréchantillonnage PCM à taux élevé : une politique de famille de fréquences plus claire**

Trois politiques au choix : famille prioritaire, famille stricte ou conversion compatible, pour une gestion limpide de l'échantillonnage. La chaîne prend en charge jusqu'à 1411.2 kHz (famille 44.1 kHz) et 1536 kHz (famille 48 kHz), selon les capacités de votre DAC et de son pilote.

### 04 · Suréchantillonnage DSD

**Jusqu'à DSD1024 : la très haute cadence traitée avec rigueur**

Conversion PCM vers DSD et conversion de cadence DSD au sein d'une même famille jusqu'à DSD1024. Modulation du 1er au 7e ordre protégée contre l'instabilité, avec transmission en ASIO DSD natif ou DoP selon votre matériel.

- Famille et ordre de modulateur modifiables ; la valeur effective après protection est toujours indiquée
- Deux voies de transmission (DoP et ASIO natif) selon les capacités du pilote
- Source DSD en pass-through direct ou décodage conditionnel au choix

## Ajustement acoustique et métadonnées musicales

`Tuning · 05–10`

Mesurer, corriger, analyser, structurer : transformer le « son agréable » en critères vérifiables.

### 05 · GPU

**AMD et NVIDIA : mobilisez votre GPU pour le calcul audio**

Double backend AMD HIP et NVIDIA CUDA pour décharger le processeur lors des calculs de suréchantillonnage et de convolution. L'interface distingue la disponibilité du GPU et son utilisation réelle sur la piste en cours.

### 06 · Analyse audio

**Dynamique, qualité et détection de transcodage réunis sur une seule vue**

Visualisez en un clin d'œil le loudness, la plage dynamique, le True Peak, l'écrêtage et le spectrogramme. Identifiez les indices de transcodage avec pertes ou de bande passante tronquée grâce à la fréquence de coupure, la raideur spectrale et l'indice de confiance.

> L'analyse lit simplement le fichier sans émettre de son de test ; les résultats fournissent des indices objectifs sans porter de jugement subjectif sur l'œuvre.

### 07 · PEQ et égalisation casque

**Égaliseur 20 bandes pour sculpter le son selon vos préférences**

Ajustez individuellement la fréquence, le gain, le facteur Q et le type de filtre sur chaque bande, avec prévisualisation graphique instantanée. Intègre les profils de calibration casque OPRA, un pré-gain automatique et un mode bypass.

### 08 · Correction acoustique de pièce

**Adaptez vos enceintes à l'acoustique de votre pièce**

Corrigez les accidents de réponse en fréquence causés par l'acoustique de votre pièce et vos enceintes grâce à l'assistant de mesure et aux filtres de correction. Compatible avec les fichiers d'étalonnage micro, les profils REW / AutoEq et l'import de réponses impulsionnelles WAV.

### 09 · Métadonnées musicales par IA

**Découvrez tout le contexte artistique derrière chaque morceau**

Grâce à l'intégration de modèles d'IA, recherchez et organisez automatiquement les fiches d'albums, de pistes, d'artistes et d'œuvres classiques. Affichage structuré et mise en cache locale pour une consultation fluide et pérenne.

> Nécessite votre propre clé d'API ; les citations, liens externes et connaissances générées par l'IA sont clairement distingués pour identifier les sources.

### 10 · Pochettes et paroles

**Donnez à votre discothèque ses visuels et ses textes**

Enrichissez automatiquement vos pochettes, paroles et métadonnées en croisant tags intégrés, images locales et bases de données en ligne. Profitez de paroles synchronisées défilantes pour une collection musicale impeccable.

## Comprendre la musique, multiplier les modes d'écoute

`Platform · 11–16`

Langues, paroles, diffusion réseau et open source : bien au-delà d'un simple lecteur local.

### 11 · Interface multilingue

**Sept langues d'interface**

Chinois, anglais, français, japonais, russe, espagnol et arabe, sélectionnables manuellement ou adaptés à la langue du système.

### 12 · Traduction des paroles

**Comprenez les paroles dans votre langue**

Traduction de paroles par IA avec translittération, conversion kana japonais et conversion sinogrammes traditionnels/simplifiés, avec texte original en regard.

### 13 · Versions officielles

**Paquets complets pour Windows et macOS**

Paquets autonomes complets disponibles pour Windows 10/11 x64 et macOS 13+ (Apple Silicon), intégrant l'environnement d'exécution, les décodeurs et les bibliothèques GPU requises. Prêt à l'emploi avec votre pilote de DAC, avec vérification des mises à jour intégrée.

### 14 · Interface et ergonomie

**Des paramètres techniques expliqués en toute clarté**

Chaque option audio clé s'accompagne d'explications contextuelles ; la chaîne de signal et les indicateurs d'état reflètent fidèlement le traitement réel.

### 15 · Diffusion sur le réseau local

**Diffusez vers vos streamers et enceintes connectées**

Diffusez vos flux audio vers les récepteurs UPnP / DLNA compatibles de votre réseau local pour intégrer votre bibliothèque PC à votre installation HiFi. La version macOS prend également en charge AirPlay.

### 16 · Sources de contenu

**Connexion EMBY, Navidrome et NAS**

Montez directement vos NAS et partages SMB/NFS, connectez les serveurs musicaux Emby, Navidrome ou Plex, et parcourez ou lisez vos bibliothèques distantes au même titre que les fichiers locaux, sans téléchargement préalable.

## Spécifications et limites techniques

`Scope`

Les spécifications de sortie maximales dépendent du matériel, du pilote et de la chaîne de traitement sélectionnée ; les paliers DoP utilisables sont limités par la fréquence de la porteuse. L'analyse audio fournit des indices objectifs ; l'IA et les métadonnées en ligne requièrent un accès réseau, une configuration de service et des sources de données valides. L'état Bit-Perfect certifie l'intégrité bit à bit des échantillons sans préjuger de la perception auditive ; le suréchantillonnage, l'égalisation et les autres traitements s'excluent mutuellement avec la transmission bit à bit.

| | |
|---|---|
| **79** | Préréglages de filtres |
| **280dB** | Atténuation cible des filtres |
| **1536kHz** | Plafond de suréchantillonnage PCM |
| **1024** | Multiplicateur DSD maximal |
| **7Ordre** | Modulateur SDM |
| **20Bandes** | Égaliseur paramétrique |
| **6230** | Profils d'égalisation casque |
| **7.1.4** | Configuration max. de correction de pièce |

## Deux documents pour tout comprendre : mode d'emploi et fondements techniques

### TECHNICAL WHITE PAPER

**Détails techniques · Livre blanc technique**

Pipeline de signal, rééchantillonnage PCM et familles de cadences, bande passante et phase des filtres, précision numérique, pass-through et stabilité de modulation DSD, CPU/GPU et limites de mesure.

→ https://rlplayer.rainlain.com/features.html

### USER GUIDE

**Mode d'emploi · Manuel officiel**

Installation et première écoute, interface et paramètres, discothèque et file de lecture, transmission bit-perfect, politiques d'échantillonnage, filtrage et dither, sortie DSD, égalisation et correction de pièce, dépannage.

→ https://rlplayer.rainlain.com/manual.html

## L'équipe du projet

| | |
|---|---|
| Développeurs du projet | RainLain, Cats and Asteroids |
| Développeurs contributeurs | Listen Storm |
| Médias et conception graphique | Gosed |
| Remerciements aux testeurs | Miyu, Downloading 99% |
| Communauté | Groupe QQ : 895816505 |
| Licence | GPL-3.0-or-later Composants tiers : voir THIRD-PARTY-NOTICES.txt dans l'archive |

## Liens

| | |
|---|---|
| Site officiel | https://rlplayer.rainlain.com |
| Télécharger | https://rlplayer.rainlain.com/download.html |
| Dernière version | https://github.com/okhuoyanyan/RLPlayer2/releases/latest |
| Retours | https://github.com/okhuoyanyan/RLPlayer-feedback/issues |
| Développement | [DEVELOPING.md](DEVELOPING.md) |

## Configuration requise

Windows 10 / 11 x64 · macOS 13+ · Portable · GPL-3.0-or-later · 7 langues d'interface

## Licence

RLPlayer Lecteur HiFi haute fidélité local pour Windows et macOS, open source sous licence GPL-3.0-or-later.
