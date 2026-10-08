# MetaGlassesPiratas 🏴‍☠️🥽

Lentes con cámara, micrófono y bocina (basados en el Seeed XIAO ESP32S3 Sense) que funcionan como un asistente de voz con IA: detectan la palabra de activación **"Jarvis"**, graban lo que dices, se lo mandan a **Gemini** (con capacidad de "ver" a través de la cámara si la pregunta lo requiere) y responden en voz alta por la bocina. Todo controlado y monitoreado desde una app móvil.

Proyecto personal, de cero: diseño del firmware, el backend y la app.

## Arquitectura

```
┌─────────────────┐   WebSocket   ┌──────────────────┐   WebSocket   ┌─────────────────┐
│   ESP32 (lentes)  │◄─────────────►│  Backend (FastAPI) │◄─────────────►│  App (Expo/RN)   │
│  cámara + mic +   │   binario +   │  + Gemini (chat/   │   JSON +      │  vista en vivo,  │
│  bocina + wake    │   JSON        │  visión/TTS)        │   base64      │  galería, control │
│  word (ESP-SR)    │               │                     │               │                   │
└──────────────────┘               └──────────────────┘               └─────────────────┘
```

1. El ESP32 escucha todo el tiempo la wake word "Jarvis" (WakeNet, on-device).
2. Al detectarla, graba el audio y lo manda al backend por WebSocket.
3. El backend le pasa el audio a Gemini. Si Gemini necesita ver algo ("¿qué es esto?"), le pide una foto al ESP32 en tiempo real (function calling) antes de responder.
4. La respuesta de texto se convierte a voz con Gemini TTS, se manda de vuelta en chunks PCM16 y el ESP32 la reproduce por la bocina.
5. La app móvil se conecta al mismo backend: ve el estado del dispositivo, pide vista en vivo, toma fotos, graba video (con el audio de la conversación incluido) y navega la galería de fotos/videos guardados.

## Estructura del repo

- **`esp32/`** — Firmware en C (ESP-IDF) para el Seeed XIAO ESP32S3 Sense: cámara, micrófono PDM, bocina I2S (MAX98357A), wake word, cliente WebSocket.
- **`server/`** — Backend en Python (FastAPI) que habla con Gemini (chat + visión + TTS), reenvía comandos entre el dispositivo y la app, y guarda fotos/video/audio.
- **`app/`** — App móvil en React Native/Expo: vista en vivo, galería de fotos y videos, control de grabación, estado del dispositivo.

Cada carpeta tiene su propio `.gitignore` y un archivo `.env.example` / `*.example` con la configuración necesaria, sin datos reales (ver abajo).

## Hardware

- Seeed Studio XIAO ESP32S3 Sense (cámara + micrófono PDM integrados)
- Bocina amplificada MAX98357A (I2S)

## Poner a correr el proyecto

### 1. `server/`

```bash
cd server
cp .env.example .env   # pon tu GEMINI_API_KEY real
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

(también hay un `docker-compose.yml` si prefieres correrlo en Docker)

### 2. `esp32/`

Con [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) instalado:

```bash
cd esp32
cp main/wifi_credentials.h.example main/wifi_credentials.h       # tu WiFi real
cp main/backend_config.h.example main/backend_config.h           # IP de tu backend en tu red local
idf.py build flash monitor
```

### 3. `app/`

```bash
cd app
cp .env.example .env   # misma IP del backend que pusiste arriba
npm install
npx expo start
```

## Configuración y datos sensibles

Nada de WiFi, API keys o IPs de red local está en el código -- todo sale de archivos `.env` / `*.h` que están en `.gitignore` y nunca se suben. Cada carpeta trae su plantilla (`.env.example`, `wifi_credentials.h.example`, `backend_config.h.example`) para que cualquiera pueda clonar el repo y correrlo con sus propios datos.

## Stack

- **Firmware:** C, ESP-IDF, ESP-SR (WakeNet), I2S (micrófono PDM + bocina), WebSocket binario
- **Backend:** Python, FastAPI, WebSockets, Gemini API (chat con function calling + visión + TTS nativo), ffmpeg (mux de audio/video)
- **App:** React Native, Expo, expo-router, expo-file-system, expo-media-library, expo-video
