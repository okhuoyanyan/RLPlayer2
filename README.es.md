# RLPlayer

[简体中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [Français](README.fr.md) · [Русский](README.ru.md) · **Español** · [العربية](README.ar.md)

**Cada etapa, concebida para escuchar música con rigor**

Sobremuestreo y filtrado de alta precisión, una cadena de audio visible y metadatos musicales en continuo perfeccionamiento. Del procesamiento digital a la escucha diaria, haciendo evolucionar el sonido junto con la experiencia.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20%C2%B7%20macOS%2013%2B-0078D6)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)
![UI](https://img.shields.io/badge/UI%20languages-7-8A2BE2)

![RLPlayer](https://rlplayer.rainlain.com/assets/img/hero/hero-01-albums.webp)

> Windows + macOS Paquetes completos para ambas plataformas 7 Idiomas de interfaz GPL-3.0 Licencia de código abierto Funcionamiento sin conexión Sin recopilación de datos

## Cadena de audio

`Capabilities · 01–04`

Decodificación, sobremuestreo, filtrado y modulación DSD: cada paso se puede ajustar en la configuración y comprobar en tiempo real en la cadena de señal.

### 01 · Cadena de audio

**Por dónde pasa el sonido, visible en cada paso**

Desde el archivo de origen hasta el dispositivo de salida, observe claramente la frecuencia de muestreo, el procesamiento de audio y el estado Bit-Perfect. La cadena de señal le explica si se conservan las muestras originales o qué ajustes alteran el sonido.

**Cadena de señal (esquema)**

- **Decodificación** — Streaming por bloques · FLAC / DSF / WAV
- **Matriz** — Downmix multicanal · 2.0 – 7.1.4
- **Ecualización** — PEQ de 20 bandas · Paso directo por bypass
- **Calibración** — Convolución IR · Corrección de sala
- **Sobremuestreo** — Hasta 1536 kHz · 79 filtros
- **Volumen** — ReplayGain · Paso directo 0 dB
- **Dither** — TPDF / NS9 · LNS15
- **Salida** — WASAPI exclusivo · ASIO / UPnP

### 02 · Filtrado

**79 preajustes de filtro para elegir con criterio**

Fase lineal, fase mínima, fase intermedia y filtrado en dos etapas para distintas estrategias de reconstrucción. Consulte las mediciones archivadas de respuesta en frecuencia y calidad para conocer el ancho de banda, la atenuación en banda eliminada y la respuesta transitoria antes de elegir el preajuste adecuado.

- Selección independiente 1x / Nx; Nx puede seguir a 1x
- La fase puede seguir el preajuste o modificarse manualmente
- Número de taps, atenuación en banda eliminada y banda de transición configurables

### 03 · Sobremuestreo PCM

**Sobremuestreo PCM de alta tasa con directrices de familia claras**

Ofrece tres estrategias: prioridad a la misma familia, misma familia estricta y conversión compatible. La cadena de procesamiento cubre 1411.2 kHz para la familia de 44.1 kHz y 1536 kHz para la de 48 kHz; la salida efectiva depende del dispositivo y del controlador.

### 04 · Sobremuestreo DSD

**Hasta DSD1024, máxima tasa tratada con rigor**

Admite conversión de PCM a DSD y conversión de tasa dentro de la misma familia DSD, hasta DSD1024. Modulación de orden 1 a 7 con protección de estabilidad, permitiendo salida DoP o DSD nativo por ASIO según el hardware.

- Familia y orden del modulador intercambiables; los valores efectivos protegidos se muestran explícitamente
- Rutas DoP y ASIO nativo según la capacidad del controlador
- Fuentes DSD con paso directo opcional o decodificación condicional

## Calibración de sonido y documentación musical

`Tuning · 05–10`

Medir, calibrar, analizar y organizar: descomponer el «suena bien» en factores verificables.

### 05 · GPU

**AMD y NVIDIA: la tarjeta gráfica al servicio del audio**

Doble backend con AMD HIP y NVIDIA CUDA para acelerar tareas compatibles de sobremuestreo y convolución. La interfaz distingue si la GPU está disponible y si la pista actual aprovecha la aceleración.

### 06 · Análisis de audio

**Dinámica, calidad y pistas de transcodificación en una sola vista**

Consulte sonoridad, rango dinámico, True Peak, recortes (clipping) y espectrograma. Mediante frecuencia de corte, pendiente espectral y nivel de confianza, detecte posibles indicios de transcodificación con pérdidas o ancho de banda recortado.

> El análisis solo lee el archivo sin reproducir tonos de prueba; los resultados aportan indicios, no juicios definitivos sobre la procedencia o calidad auditiva.

### 07 · PEQ y EQ para auriculares

**Ecualización de 20 bandas para moldear el sonido a su gusto**

Ajuste independiente de frecuencia, ganancia, valor Q y tipo de filtro en cada banda, con respuesta instantánea al arrastrar curvas. Compatible con preajustes de calibración de auriculares OPRA, preatenuación automática y bypass para conciliar precisión y preferencias personales.

### 08 · Corrección acústica de sala

**Adapte sus altavoces al espacio de escucha**

Corrija desviaciones de respuesta en frecuencia causadas conjuntamente por la sala y los altavoces mediante un asistente de medición y filtros de corrección. Admite archivos de calibración de micrófono, ecualizaciones paramétricas de REW / AutoEq e importación de respuestas impulsionales WAV.

### 09 · Información musical asistida por IA

**Descubra el contexto musical tras cada canción**

Conecte la IA para buscar y estructurar datos de álbumes, canciones, artistas y obras. La presentación organizada y la caché local facilitan consultar y releer el contexto de su música.

> Requiere clave de API propia. Citas, enlaces externos y «Conocimiento IA» se identifican por separado para distinguir claramente la fuente.

### 10 · Portadas y letras

**Una biblioteca con imagen visual e historia**

Complete portadas, letras y metadatos combinando etiquetas incrustadas, imágenes locales y fuentes en línea. Compatible con letras sincronizadas y organización de información para que su colección esté completa y ordenada.

## Comprender la música y ampliar las formas de escuchar

`Platform · 11–16`

Idiomas, letras, streaming y código abierto: más allá del reproductor, un ecosistema a su alcance.

### 11 · Interfaz multilingüe

**Siete idiomas de interfaz**

Chino, inglés, francés, japonés, ruso, español y árabe, seleccionables manualmente o según el sistema.

### 12 · Traducción de letras

**Entender cada verso**

Traducción de letras por IA con transliteración, furigana/kana japonés y conversión tradicional/simplificada, conservando el texto original.

### 13 · Versión oficial

**Paquetes completos para Windows y macOS**

Paquetes completos listos para Windows 10/11 x64 y macOS 13+ (Apple Silicon), con entorno visual, decodificadores y componentes de GPU integrados; listos para usar con el controlador del dispositivo y actualizables desde la aplicación.

### 14 · Interfaz e interacción

**Ajustes complejos explicados con claridad**

Las opciones clave de audio incluyen explicaciones detalladas; la cadena de señal y los avisos de estado muestran el procesamiento real.

### 15 · Transmisión en red local

**Lleve la música a sus reproductores de red y altavoces**

Transmita a dispositivos compatibles con UPnP/DLNA en su red local e integre la biblioteca de su ordenador en su equipo existente; la versión para macOS también admite AirPlay.

### 16 · Fuentes de contenido

**Conexión con EMBY, Navidrome y NAS**

Monte directamente NAS y recursos compartidos SMB/NFS, conecte servidores musicales Emby, Navidrome o Plex y navegue o reproduzca bibliotecas remotas igual que los archivos locales, sin descargarlas antes.

## Especificaciones y límites técnicos

`Scope`

Las especificaciones máximas de salida dependen del dispositivo, el controlador y la ruta elegida; los modos DoP utilizables están limitados por la frecuencia de la portadora. El análisis de audio aporta indicios; las funciones de IA e información en línea dependen de la red, la configuración del servicio y las fuentes de datos. Bit-Perfect indica que las muestras permanecen inalteradas, sin prejuzgar la apreciación auditiva; el sobremuestreo, la ecualización y otros procesos no pueden coexistir con el paso directo bit a bit.

| | |
|---|---|
| **79** | Preajustes de filtrado |
| **280dB** | Rechazo objetivo del filtro |
| **1536kHz** | Límite superior de sobremuestreo PCM |
| **1024** | Tasa máxima DSD |
| **7Órdenes** | Modulador SDM |
| **20Bandas** | Ecualizador paramétrico |
| **6230** | Ajustes de calibración de auriculares |
| **7.1.4** | Configuración máxima de sala |

## Dos documentos técnicos: cómo usarlo y los fundamentos de diseño

### TECHNICAL WHITE PAPER

**Detalles técnicos · Libro blanco**

Cadena de señal, remuestreo PCM y familias de frecuencias, ancho de banda y fase de filtros, precisión numérica, paso directo y estabilidad de modulación DSD, procesamiento CPU/GPU y límites de medición.

→ https://rlplayer.rainlain.com/features.html

### USER GUIDE

**Instrucciones de uso · Manual oficial**

Instalación y primera reproducción, interfaz y configuración, biblioteca y cola, paso directo bit-perfect, estrategias de muestreo, filtrado y dither, salida DSD, EQ y corrección de sala, solución de problemas.

→ https://rlplayer.rainlain.com/manual.html

## Quiénes lo hacen posible

| | |
|---|---|
| Desarrollador del proyecto | RainLain, Cats and Asteroids |
| Desarrollador colaborador | Listen Storm |
| Medios y arte | Gosed |
| Agradecimientos por las pruebas | Miyu, Downloading 99% |
| Comunidad | Grupo de QQ 895816505 |
| Licencia | GPL-3.0-or-later Componentes de terceros detallados en THIRD-PARTY-NOTICES.txt dentro del paquete |

## Enlaces

| | |
|---|---|
| Sitio oficial | https://rlplayer.rainlain.com |
| Descargar | https://rlplayer.rainlain.com/download.html |
| Última versión | https://github.com/okhuoyanyan/RLPlayer2/releases/latest |
| Comentarios | https://github.com/okhuoyanyan/RLPlayer-feedback/issues |
| Desarrollo | [DEVELOPING.md](DEVELOPING.md) |

## Requisitos del sistema

Windows 10 / 11 x64 · macOS 13+ · Portátil sin instalación · GPL-3.0-or-later · 7 idiomas de interfaz

## Licencia

RLPlayer Reproductor HiFi local de alta fidelidad. Para Windows y macOS, código abierto GPL-3.0-or-later.
