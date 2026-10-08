"""Protocolo binario simple entre el ESP32 y el backend.

Los mensajes de control van como texto JSON (ver models.py).
Los binarios (fotos, frames de video, audio) van como bytes crudos
con UN byte al inicio que dice de qué tipo es, para no tener que
abrir dos WebSockets distintos.
"""
from enum import IntEnum

class BinaryType(IntEnum):
    PHOTO = 1          # foto completa (JPEG)
    VIDEO_FRAME = 2    # un frame JPEG mientras se está grabando
    LIVE_FRAME = 3     # un frame JPEG de la vista en vivo (no se guarda)
    MIC_AUDIO = 4       # chunk de audio del micrófono (lo usamos en el paso de Gemini)
    TTS_AUDIO = 5        # chunk de audio de respuesta para reproducir (idem)
    RECORDING_AUDIO = 6
    
def pack(kind: BinaryType, data: bytes) -> bytes:
    return bytes([kind]) + data

def unpack(raw: bytes) -> tuple[BinaryType, bytes]:
    return BinaryType(raw[0]), raw[1:]