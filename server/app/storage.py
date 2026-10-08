"""Guardado local de fotos y videos recibidos del ESP32.

Por ahora todo se guarda en disco, dentro de MEDIA_DIR. La app móvil
los descarga desde aquí; más adelante esto se puede apuntar a un
bucket en la nube sin cambiar el resto del código.
"""
import wave
from datetime import datetime
from pathlib import Path
from app.config import settings
import subprocess

def _device_dir(device_id: str, kind: str) -> Path:
    d = Path(settings.media_dir) / device_id / kind
    d.mkdir(parents=True, exist_ok=True)
    return d

def save_photo(device_id: str, data: bytes) -> Path:
    timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S_%f")
    path = _device_dir(device_id,"photos") / f"{timestamp}.jpg"
    path.write_bytes(data)
    return path

def open_video_writer(device_id: str):
    """Abre un archivo donde se agrega los frames JPEG/MJPEG mientras se graba"""
    timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S_%f")
    path = _device_dir(device_id, "videos") / f"{timestamp}.mjpeg"
    return path.open("ab"), path

def save_mic_audio(device_id: str, pcm_data: bytes, sample_rate: int = 16000) -> Path:
    """Guarda audio crudo PCM16 mono (lo que manda el ESP32 tras detectar la
    wake word) como un .wav, para poder escucharlo y despues mandarlo a Gemini."""
    timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S_%f")
    path = _device_dir(device_id, "audio") / f"{timestamp}.wav"
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16 bits
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data)
    return path

def convert_mjpeg_to_mp4(mjpeg_path: Path) -> Path | None:
    """Convierte el .mjpeg grabado a un .mp4 reproducible en celulares.
    Si algo falla, deja el .mjpeg intacto para no perder el material."""
    mp4_path = mjpeg_path.with_suffix(".mp4")
    try:
        subprocess.run(
            [
                "ffmpeg", "-y",
                "-f", "mjpeg",
                "-r", "20/3",  # fps de ENTRADA: el ESP32 manda un frame cada ~150ms (20/3 ≈ 6.67 fps)
                "-i", str(mjpeg_path),
                "-c:v", "libx264", "-pix_fmt", "yuv420p",
                "-movflags", "+faststart",  # mueve el indice al inicio para que se pueda reproducir via streaming/HTTP
                str(mp4_path),
            ],
            check=True,
            capture_output=True,
        )
        mjpeg_path.unlink()
        return mp4_path
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None

def save_pcm_to_wav(pcm_data: bytes, path: Path, sample_rate: int = 16000) -> None:
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data)

def mux_audio_into_video(video_path: Path, wav_path: Path) -> bool:
    """Combina el video (sin audio) con la pista de audio grabada, y
    reemplaza video_path con la version que ya trae audio."""
    tmp_path = video_path.with_name(video_path.stem + "_with_audio.mp4")
    try:
        subprocess.run(
            [
                "ffmpeg", "-y",
                "-i", str(video_path),
                "-i", str(wav_path),
                "-c:v", "copy",
                "-c:a", "aac",
                "-shortest",
                str(tmp_path),
            ],
            check=True,
            capture_output=True,
        )
        tmp_path.replace(video_path)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False