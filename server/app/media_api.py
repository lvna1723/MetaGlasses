"""Endpoints REST para que la app liste y descargue fotos/videos ya
guardados (la galería). Es aparte del WebSocket porque esto es una
consulta simple, no tiempo real."""
from pathlib import Path
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse, StreamingResponse
from app.config import settings

router = APIRouter()

CHUNK_SIZE = 1024 * 1024  # 1 MB por pedazo al transmitir

def _media_root(device_id: str) -> Path:
    return Path(settings.media_dir) / device_id

@router.get("/media/{device_id}")
def list_media(device_id: str):
    root = _media_root(device_id)
    items = []
    for kind, ext in (("photos", ".jpg"), ("videos", ".mp4")):
        folder = root / kind
        if not folder.exists():
            continue
        for f in sorted(folder.glob(f"*{ext}"), reverse=True):
            items.append({
                "filename": f.name,
                "kind": "photo" if kind == "photos" else "video",
                "url": f"/media/{device_id}/{kind}/{f.name}",
                "size_bytes": f.stat().st_size,
            })
    return {"items": items}

@router.get("/media/{device_id}/{kind}/{filename}")
def get_media_file(device_id: str, kind: str, filename: str, request: Request):
    if kind not in ("photos", "videos"):
        raise HTTPException(404)
    path = _media_root(device_id) / kind / filename
    if not path.resolve().is_relative_to(_media_root(device_id).resolve()):
        raise HTTPException(400, "Ruta inválida")
    if not path.exists():
        raise HTTPException(404, "No encontrado")
    
    media_type = "image/jpeg" if kind == "photos" else "video/mp4"
    file_size = path.stat().st_size
    range_header = request.headers.get("range")
    
    # AVPlayer (iOS) manda un Range chiquito para "probar" el archivo antes
    # de reproducirlo. Starlette (la versión que trae este FastAPI) no
    # soporta Range en FileResponse, así que lo manejamos a mano.
    if range_header:
        try:
            range_value = range_header.strip().split("=")[-1]
            start_str, end_str = range_value.split("-")
            start = int(start_str)
            end = int(end_str) if end_str else file_size - 1
            end = min(end, file_size - 1)
            chunk_len = end - start + 1

            def iter_chunk():
                with path.open("rb") as f:
                    f.seek(start)
                    remaining = chunk_len
                    while remaining > 0:
                        data = f.read(min(CHUNK_SIZE, remaining))
                        if not data:
                            break
                        remaining -= len(data)
                        yield data

            headers = {
                "Content-Range": f"bytes {start}-{end}/{file_size}",
                "Accept-Ranges": "bytes",
                "Content-Length": str(chunk_len),
            }
            return StreamingResponse(iter_chunk(), status_code=206, media_type=media_type, headers=headers)
        except (ValueError, IndexError):
            pass  # Range inválido -- seguimos abajo y mandamos el archivo completo

    response = FileResponse(path, media_type=media_type)
    response.headers["Accept-Ranges"] = "bytes"
    return response