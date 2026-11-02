# RLPlayer

[简体中文](README.md) · **English** · [日本語](README.ja.md) · [Français](README.fr.md) · [Русский](README.ru.md) · [Español](README.es.md) · [العربية](README.ar.md)

**Every link, Crafted for dedicated music listening**

High-precision upsampling and filtering, transparent audio signal paths, and continuously refined music metadata. From digital processing to daily listening, elevating sound and experience together.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20%C2%B7%20macOS%2013%2B-0078D6)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)
![UI](https://img.shields.io/badge/UI%20languages-7-8A2BE2)

![RLPlayer](https://rlplayer.rainlain.com/assets/img/hero/hero-01-albums.webp)

> Windows + macOS Dual-Platform Full Bundle 7 UI Languages GPL-3.0 Open Source License Offline Capable Zero Data Collection

## Audio Signal Path

`Capabilities · 01–04`

Decoding, upsampling, filtering, and DSD modulation—customize every stage in Settings, and track exactly what it is doing in the live signal path.

### 01 · Audio Signal Path

**Transparent processing—every step clearly visible**

From source file to output endpoint, intuitively inspect sample rate, active processing, and Bit-Perfect status. See whether raw samples are preserved and which settings alter the sound directly in the signal path.

**Signal chain (illustrative)**

- **Decoding** — Stream chunking · FLAC / DSF / WAV
- **Matrix** — Multichannel downmix · 2.0 – 7.1.4
- **Equalization** — 20-band PEQ · Bypassable bit-perfect
- **Calibration** — Convolution IR · Room correction
- **Upsampling** — Up to 1536 kHz · 79 filters
- **Volume** — ReplayGain · 0 dB bit-perfect
- **Dither** — TPDF / NS9 · LNS15
- **Output** — WASAPI Exclusive · ASIO / UPnP

### 02 · Filtering

**79 filter presets—make choices backed by data**

Linear phase, minimum phase, intermediate phase, and two-stage filtering offer diverse reconstruction characteristics. Backed by archived frequency response and quality measurements, examine bandwidth, stopband attenuation, and transient response to find your ideal preset.

- Independent 1x / Nx selection; Nx can follow 1x
- Phase can follow preset or be manually overridden
- Customizable taps, stopband attenuation, and transition band

### 03 · PCM Upsampling

**High-rate PCM upsampling with clear family-rate policies**

Offers Prefer Same Family, Strict Same Family, and Compatible Conversion policies for transparent upsampling choices. Processing pipelines cover 1411.2 kHz for the 44.1 kHz family and 1536 kHz for the 48 kHz family; actual output depends on your device and driver.

### 04 · DSD Upsampling

**Up to DSD1024—rigorous high-rate modulation**

Supports PCM-to-DSD and same-family DSD rate conversion up to DSD1024. 1st–7th order modulation combined with stability safeguards, supporting ASIO Native DSD or DoP transmission tailored to device capabilities.

- Modulator family and order are configurable; safeguarded effective values are explicitly reported
- Dual DoP and ASIO Native paths, chosen based on driver capabilities
- DSD sources support bit-perfect passthrough or conditional decoding

## Acoustic Tuning & Music Metadata

`Tuning · 05–10`

Measurement, calibration, analysis, and curation—breaking down "sounds good" into verifiable elements.

### 05 · GPU

**AMD & NVIDIA—harness GPUs for audio computing**

Dual AMD HIP and NVIDIA CUDA backends offload compute for supported upsampling and convolution tasks. The UI clearly distinguishes GPU availability from whether the current track is actively hardware-accelerated.

### 06 · Audio Analysis

**Dynamics, quality, and transcoding clues—all on one page**

Centrally inspect loudness, dynamic range, true peak, clipping, and spectral analysis. Evaluates cutoff frequencies, spectral roll-off steepness, and confidence levels to highlight clues of lossy transcoding or bandwidth limits.

> Analysis operates strictly read-only without playing test tones; results provide investigative clues, not definitive verdicts on provenance or audio quality.

### 07 · PEQ & Headphone EQ

**20-band EQ—tailor the sound to your preference**

Independently adjust frequency, gain, Q factor, and filter type per band with real-time responsive curve dragging. Integrated with OPRA headphone calibration presets, auto-preamp, and bypass to balance correction with personal taste.

### 08 · Room Acoustic Correction

**Tailor your speakers to your room acoustics**

Compensate for frequency response anomalies caused by room acoustics and speaker placement via a guided measurement wizard and correction filters. Supports microphone calibration files, REW / AutoEq parametric EQ text, and WAV impulse responses.

### 09 · AI Music Metadata

**Discover deeper context behind every track**

Integrated AI assists in searching and curating album, track, artist, and composition metadata. Structured presentation and local caching make musical backgrounds effortless to discover and revisit.

> Requires your own API key; citations, external links, and "AI Knowledge" are distinctly labeled to clarify source provenance.

### 10 · Covers & Lyrics

**Bring album art and stories to your library**

Combines embedded audio tags, local images, and online data sources to complete covers, lyrics, and track information. Supports synchronized scrolling lyrics and metadata curation to keep your collection complete and organized.

## Understand Music, Connect More Ways to Listen

`Platform · 11–16`

Languages, lyrics, streaming, and open source—exploring the frontiers beyond the player.

### 11 · Multilingual Interface

**Seven UI Languages**

Chinese, English, French, Japanese, Russian, Spanish, and Arabic—switch manually or follow your system language.

### 12 · Lyric Translation

**Understand Every Lyric**

AI lyric translation with romanization, Japanese furigana, and traditional/simplified conversion, presented alongside original lyrics.

### 13 · Official Release

**Self-Contained Windows & macOS Bundles**

Full standalone bundles for Windows 10/11 x64 and macOS 13+ (Apple Silicon), built-in with web runtime, decoders, and required GPU libraries. Ready to run with standard device drivers, supporting in-app update checks.

### 14 · UI & Interaction

**Demystifying complex audio settings**

Key audio options feature clear explanations, while the signal path and status indicators present actual real-time processing results.

### 15 · Local Network Streaming

**Stream directly to your streamers and speakers**

Cast music to UPnP / DLNA compatible devices on your local network to integrate your PC library into existing audio systems; macOS version additionally supports AirPlay casting.

### 16 · Content sources

**Connect EMBY, Navidrome and NAS**

Mount NAS drives and SMB/NFS shares directly, connect Emby, Navidrome and Plex music servers, and browse or play cloud libraries alongside local files without downloading them first.

## Specifications & Limits

`Scope`

Maximum output specifications depend on device, driver, and selected processing path; available DoP rates are limited by carrier frequency. Audio analysis provides investigative clues; AI and online metadata features rely on network connectivity, service configuration, and data sources. Bit-Perfect indicates sample integrity, not a subjective sound quality rating; active upsampling, EQ, and bit-perfect passthrough are mutually exclusive.

| | |
|---|---|
| **79** | Filter Presets |
| **280dB** | Target Filter Rejection |
| **1536kHz** | Max PCM Upsampling |
| **1024** | Max DSD Rate |
| **7th-Order** | SDM Modulator |
| **20-Band** | Parametric EQ |
| **6230** | Headphone EQ Profiles |
| **7.1.4** | Max Room Correction Layout |

## Two In-Depth Documents: How It Works & Why It Was Designed This Way

### TECHNICAL WHITE PAPER

**Technical Details · Technical Whitepaper**

Signal pipeline, PCM resampling and sample rate families, filter bandwidth and phase, numerical precision, DSD passthrough and modulation stability, CPU/GPU processing, and measurement boundaries.

→ https://rlplayer.rainlain.com/features.html

### USER GUIDE

**User Manual · Official Guide**

Installation and first playback, interface and settings, library and queues, bit-perfect passthrough, sample rate policies, filtering and dither, DSD output, EQ and room correction, troubleshooting.

→ https://rlplayer.rainlain.com/manual.html

## People Behind the Project

| | |
|---|---|
| Project Developer | RainLain, Cats and Asteroids |
| Contributing Developer | Listen Storm |
| Media & Artwork | Gosed |
| Testing Credits | Miyu, Downloading 99% |
| Community | QQ Group 895816505 |
| License | GPL-3.0-or-later Third-party components listed in THIRD-PARTY-NOTICES.txt in the bundle |

## Links

| | |
|---|---|
| Website | https://rlplayer.rainlain.com |
| Download | https://rlplayer.rainlain.com/download.html |
| Latest release | https://github.com/okhuoyanyan/RLPlayer2/releases/latest |
| Feedback | https://github.com/okhuoyanyan/RLPlayer-feedback/issues |
| Development | [DEVELOPING.md](DEVELOPING.md) |

## System requirements

Windows 10 / 11 x64 · macOS 13+ · Portable · GPL-3.0-or-later · 7 UI Languages

## License

RLPlayer Local HiFi Audio Player. Dual-platform Windows & macOS, open source under GPL-3.0-or-later.
