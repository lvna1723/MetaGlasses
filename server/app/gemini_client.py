"""Integracion con Gemini: le mandamos el audio grabado tras la wake
word y regresamos su respuesta en texto. Si Gemini pide "ver_camara",
le tomamos una foto al ESP32 (via request_photo en ws_device) y se la
mandamos de vuelta antes de la respuesta final.
"""
import asyncio
import audioop
import base64
import logging
import time
import re
from pathlib import Path
from google import genai
from app.config import settings


logger = logging.getLogger("gemini")

MODEL = "gemini-3.5-flash"

SYSTEM_PROMPT = (
    "Eres el asistente de voz de unos lentes con camara. Respondes de forma breve y natural, como en una conversacion "
    "hablada -- sin listas, sin markdown, sin emojis, porque tu respuesta "
    "se va a convertir a voz. Si para responder bien necesitas ver lo que "
    "la persona tiene enfrente en este momento, usa la funcion ver_camara."
)

VER_CAMARA_TOOL = {
    "type": "function",
    "name": "ver_camara",
    "description": (
        "Toma una foto con la camara de los lentes para ver lo que la "
        "persona tiene enfrente ahora mismo. Usala cuando la pregunta "
        "dependa de algo visual (que es esto, de que color es, que dice "
        "aqui, cuantos hay, etc)."
    ),
    "parameters": {"type": "object", "properties": {}},
}

_client = None


def _get_client():
    global _client
    if _client is None:
        _client = genai.Client(api_key=settings.gemini_api_key)
    return _client


async def ask_gemini(device_id: str, wav_path: Path) -> str:
    from app.ws_device import request_photo  # import tardio: evita ciclo de imports

    client = _get_client()
    audio_b64 = base64.b64encode(wav_path.read_bytes()).decode("utf-8")

    t0 = time.monotonic()
    interaction = await asyncio.to_thread(
        client.interactions.create,
        model=MODEL,
        system_instruction=SYSTEM_PROMPT,
        input=[{"type": "audio", "data": audio_b64, "mime_type": "audio/wav"}],
        tools=[VER_CAMARA_TOOL],
    )
    logger.info("Gemini (audio -> texto) tardo %.1fs", time.monotonic() - t0)

    # Por si Gemini pide la camara mas de una vez; tope para no atorarnos.
    for _ in range(3):
        fc_step = next((s for s in interaction.steps if s.type == "function_call"), None)
        if fc_step is None:
            break

        logger.info("Gemini pidio ver_camara (%s)", device_id)
        photo_bytes = await request_photo(device_id)

        if photo_bytes is None:
            result = [{"type": "text", "text": "No se pudo tomar la foto (camara no disponible)."}]
        else:
            result = [
                {"type": "text", "text": "Foto tomada con la camara de los lentes."},
                {
                    "type": "image",
                    "data": base64.b64encode(photo_bytes).decode("utf-8"),
                    "mime_type": "image/jpeg",
                },
            ]

        t1 = time.monotonic()
        interaction = await asyncio.to_thread(
            client.interactions.create,
            model=MODEL,
            input=[{
                "type": "function_result",
                "name": fc_step.name,
                "call_id": fc_step.id,
                "result": result,
            }],
            tools=[VER_CAMARA_TOOL],
            previous_interaction_id=interaction.id,
        )
        logger.info("Gemini (function_result -> texto) tardo %.1fs", time.monotonic() - t1)

    text = (interaction.output_text or "").strip()
    if not text:
        text = "No se me ocurrio que responder."
    logger.info("Respuesta de Gemini para %s: %s", device_id, text)
    return text


TTS_MODEL = "gemini-2.5-flash-preview-tts"
TTS_VOICE = "Kore"
TTS_SOURCE_RATE = 24000  # lo que entrega Gemini TTS
TTS_TARGET_RATE = 16000  # lo que espera la bocina del ESP32 (I2S_SAMPLE_RATE)

_TIMESTAMP_PATTERN = re.compile(r"\b\d{1,2}:\d{2}(?::\d{2})?\b")

def _sanitize_for_tts(text: str) -> str:
    """A veces Gemini mete cosas raras como marcas de tiempo (ej. '00:20:00')
    en medio de la respuesta -- parece disparar el filtro de contenido de la
    API de TTS. Las quitamos antes de mandar el texto a sintetizar."""
    cleaned = _TIMESTAMP_PATTERN.sub("", text)
    cleaned = re.sub(r"\s{2,}", " ", cleaned).strip()
    return cleaned or text

async def synthesize_speech(text: str) -> bytes:
    """Convierte texto a voz con Gemini TTS y regresa PCM16 mono a 16kHz
    -- la misma tasa que usa la bocina del ESP32 -- listo para mandar."""
    client = _get_client()
    
    text = _sanitize_for_tts(text)

    t0 = time.monotonic()
    interaction = await asyncio.to_thread(
        client.interactions.create,
        model=TTS_MODEL,
        input=text,
        response_format={"type": "audio"},
        generation_config={"speech_config": [{"voice": TTS_VOICE}]},
    )
    logger.info("Gemini TTS tardo %.1fs", time.monotonic() - t0)

    pcm_24k = base64.b64decode(interaction.output_audio.data)
    pcm_16k, _ = audioop.ratecv(pcm_24k, 2, 1, TTS_SOURCE_RATE, TTS_TARGET_RATE, None)
    return pcm_16k
