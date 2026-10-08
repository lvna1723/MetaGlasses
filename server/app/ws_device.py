"""WebSocket que usa el ESP32 para conectarse al backend.

Un dispositivo se conecta a  ws://<host>:8000/ws/device/{device_id}
y a partir de ahí:
  - manda mensajes JSON de texto (eventos, "hello", etc.)
  - manda mensajes binarios (fotos, frames de video/live view)
  - recibe mensajes JSON de texto (comandos: take_photo, etc.)
"""
import asyncio
import base64
import json
import logging
from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from app.connections import manager
from app.protocol import BinaryType, pack, unpack
from app.storage import save_photo

logger = logging.getLogger("device_ws")
router = APIRouter()

# Mientras un dispositivo está grabando, guardamos aquí su archivo abierto.
_recording_files: dict[str, tuple] = {}
_mic_audio_buffers: dict[str, bytearray] = {}
_recording_audio_buffers: dict[str, bytearray] = {}
_pending_photo_futures: dict[str, "asyncio.Future"] = {}

@router.websocket("/ws/cam/{device_id}")
async def device_socket(websocket: WebSocket, device_id: str):
    await manager.connect_device(device_id, websocket)
    logger.info("Dispositivo conectado: %s",device_id)
    await _notify_device_status(device_id, True)   
    try:
        while True:
            message = await websocket.receive()
            
            if message["type"] == "websocket.disconnect":
                raise WebSocketDisconnect(message.get("code", 1000))
            
            if message.get("text") is not None:
                await _handle_text(device_id, message["text"])
                
            elif message.get("bytes") is not None:
                await _handle_binary(device_id, message["bytes"])
                
    except WebSocketDisconnect:
        logger.info("Dispositivo desconectado: %s", device_id)
        manager.disconnect_device(device_id)
        if device_id in _recording_files:
            logger.warning("El dispositivo se desconectó mientras grababa, cerrando el video con lo que alcanzó a grabar")
            close_recording(device_id)
        await _notify_device_status(device_id, False) 
        
async def _handle_text(device_id: str, raw_text: str) -> None:
    try:
        data = json.loads(raw_text)
    except json.JSONDecodeError:
        logger.warning("JSON invalido del dispositivo %s: %s", device_id, raw_text)
        return
    
    msg_type = data.get("type")
    logger.info("Evento de %s: %s", device_id, msg_type)
    
    if msg_type == "hello":
        # Handshake interno del dispositivo, no le interesa a la app.
        logger.info("%s dice hola. payload=%s", device_id, data.get("payload"))
        return
    
    if msg_type == "wake_word_detected":
        # Aquí engancharemos la conversación con Gemini en el siguiente paso.
        logger.info("Wake word detectada en %s", device_id)

    if msg_type == "mic_audio_start":
        _mic_audio_buffers[device_id] = bytearray()
        logger.info("Inicio de grabación de audio: %s", device_id)

    if msg_type == "mic_audio_end":
        buf = _mic_audio_buffers.pop(device_id, None)
        if buf:
            from app.storage import save_mic_audio
            path = save_mic_audio(device_id, bytes(buf))
            logger.info("Audio guardado: %s (%d bytes)", path, len(buf))
            for viewer in list(manager.viewers.get(device_id, [])):
                await viewer.send_text(json.dumps({"type": "mic_audio_saved", "payload": {"path": str(path)}}))
            asyncio.create_task(_run_gemini(device_id, path))
        else:
            logger.warning("mic_audio_end sin buffer previo para %s", device_id)

    # El resto de eventos sí le interesan a la app (wake word, fin de grabación, etc.)
    for viewer in list(manager.viewers.get(device_id, [])):
        await viewer.send_text(raw_text)

async def _handle_binary(device_id: str, raw: bytes) -> None:
    kind, data = unpack(raw)

    if kind == BinaryType.PHOTO:
        path = save_photo(device_id, data)
        logger.info("Foto guardada: %s (%d bytes)", path, len(data))

        # Si Gemini esta esperando esta foto para "ver_camara", se la damos.
        future = _pending_photo_futures.get(device_id)
        if future is not None and not future.done():
            future.set_result(data)

        # avisamos a la app que hay una foto nueva
        for viewer in list(manager.viewers.get(device_id, [])):
            await viewer.send_text(json.dumps({"type": "photo_saved", "payload": {"path": str(path)}}))

    elif kind == BinaryType.MIC_AUDIO:
        buf = _mic_audio_buffers.get(device_id)
        if buf is not None:
            buf.extend(data)

    elif kind == BinaryType.VIDEO_FRAME:
        if device_id not in _recording_files:
            _recording_files[device_id] = _open_recording(device_id)
        f, path = _recording_files[device_id]
        f.write(data)

    elif kind == BinaryType.LIVE_FRAME:
        # Vista en vivo: NO se guarda. La mandamos como texto JSON con la
        # imagen en base64 -- React Native maneja fatal los WebSockets
        # binarios, pero JSON+base64 funciona sin complicaciones.
        b64 = base64.b64encode(data).decode("ascii")
        frame_msg = json.dumps({"type": "live_frame", "payload": {"data": b64}})
        for viewer in list(manager.viewers.get(device_id, [])):
            await viewer.send_text(frame_msg)
    
    elif kind == BinaryType.RECORDING_AUDIO:
        buf = _recording_audio_buffers.get(device_id)
        if buf is not None:
            buf.extend(data)

async def _notify_device_status(device_id: str, connected: bool) -> None:
    msg = json.dumps({"type": "device_status", "payload": {"connected": connected}})
    for viewer in list(manager.viewers.get(device_id, [])):
        await viewer.send_text(msg)
        
def _open_recording(device_id: str):
    from app.storage import open_video_writer
    return open_video_writer(device_id)

def close_recording(device_id: str) -> None:
    entry = _recording_files.pop(device_id, None)
    audio_buf = _recording_audio_buffers.pop(device_id, None)
    if entry:
        f, path = entry
        f.close()
        logger.info("Video guardado: %s", path)
        from app.storage import convert_mjpeg_to_mp4, save_pcm_to_wav, mux_audio_into_video
        mp4_path = convert_mjpeg_to_mp4(path)
        if mp4_path:
            logger.info("Video convertido: %s", mp4_path)
            if audio_buf:
                wav_path = mp4_path.with_suffix(".wav")
                save_pcm_to_wav(bytes(audio_buf), wav_path)
                if mux_audio_into_video(mp4_path, wav_path):
                    logger.info("Audio agregado al video: %s", mp4_path)
                else:
                    logger.warning("No se pudo agregar el audio al video %s", mp4_path)
                wav_path.unlink(missing_ok=True)
        else:
            logger.warning("No se pudo convertir %s a mp4 (¿ffmpeg instalado?)", path)

def is_recording(device_id: str) -> bool:
    return device_id in _recording_files

def start_recording_audio(device_id: str) -> None:
    _recording_audio_buffers[device_id] = bytearray()

async def request_photo(device_id: str, timeout: float = 8.0):
    """Le pide una foto al ESP32 y espera a que llegue (la usa Gemini para
    la funcion ver_camara). Regresa los bytes JPEG, o None si el
    dispositivo no esta conectado o no llega a tiempo."""
    device_ws = manager.get_device(device_id)
    if device_ws is None:
        return None

    loop = asyncio.get_event_loop()
    future = loop.create_future()
    _pending_photo_futures[device_id] = future

    await device_ws.send_text(json.dumps({"type": "take_photo", "payload": {}}))

    try:
        return await asyncio.wait_for(future, timeout=timeout)
    except asyncio.TimeoutError:
        logger.warning("Timeout esperando la foto de %s", device_id)
        return None
    finally:
        _pending_photo_futures.pop(device_id, None)


async def _run_gemini(device_id: str, wav_path) -> None:
    from app.gemini_client import ask_gemini, synthesize_speech
    logger.info("Lanzando tarea de Gemini para %s (%s)", device_id, wav_path)
    try:
        text = await ask_gemini(device_id, wav_path)
    except Exception:
        logger.exception("Error llamando a Gemini para %s", device_id)
        text = "Hubo un error hablando con Gemini."

    for viewer in list(manager.viewers.get(device_id, [])):
        await viewer.send_text(json.dumps({"type": "gemini_response", "payload": {"text": text}}))

    try:
        pcm = await synthesize_speech(text)
    except Exception:
        # Gemini TTS a veces falla con "Model tried to generate text" de forma
        # intermitente (el modelo se "confunde" y trata de responder en vez de
        # narrar) -- antes de rendirnos con el mensaje generico, probamos una
        # vez mas con el mismo texto.
        logger.warning("Error generando el audio TTS para %s, reintentando una vez", device_id)
        try:
            pcm = await synthesize_speech(text)
        except Exception:
            logger.exception("Error generando el audio TTS para %s tras reintentar, uso mensaje generico", device_id)
            try:
                pcm = await synthesize_speech("Lo siento, no puedo leer esa respuesta en voz alta en este momento.")
            except Exception:
                logger.exception("Tampoco se pudo generar el mensaje generico de audio para %s", device_id)
                return

    try:
        await send_tts_audio(device_id, pcm)
    except Exception:
        logger.exception("Error mandando el audio TTS para %s", device_id)


async def _wait_for_device(device_id: str, timeout: float = 10.0):
    """Gemini puede tardar bastante (varios a decenas de segundos), y en
    ese rato el ESP32 se pudo haber reconectado (WiFi, timeout de
    ping/pong, etc). Le damos un ratito a que vuelva a aparecer antes de
    darnos por vencidos."""
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout
    while True:
        device_ws = manager.get_device(device_id)
        if device_ws is not None:
            return device_ws
        if loop.time() >= deadline:
            return None
        await asyncio.sleep(0.5)


async def send_tts_audio(device_id: str, pcm16_bytes: bytes, chunk_samples: int = 1024) -> None:
    """Manda audio PCM16 mono a 16kHz al ESP32 en pedacitos, como frames
    TTS_AUDIO, para que los vaya reproduciendo por la bocina."""
    device_ws = await _wait_for_device(device_id)
    if device_ws is None:
        logger.warning("No se puede mandar audio TTS, %s no esta conectado", device_id)
        return

    chunk_bytes = chunk_samples * 2  # 16 bits = 2 bytes por muestra
    for i in range(0, len(pcm16_bytes), chunk_bytes):
        chunk = pcm16_bytes[i:i + chunk_bytes]
        await device_ws.send_bytes(pack(BinaryType.TTS_AUDIO, chunk))
        # Si en este momento se está grabando un video, la respuesta de
        # Gemini también entra al audio del video (además de reproducirse
        # por la bocina).
        rec_buf = _recording_audio_buffers.get(device_id)
        if rec_buf is not None:
            rec_buf.extend(chunk)

    logger.info("Audio TTS mandado a %s (%d bytes)", device_id, len(pcm16_bytes))
