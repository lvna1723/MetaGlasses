"""WebSocket que usa la app móvil para controlar un dispositivo.

La app se conecta a  ws://<host>:8000/ws/app/{device_id}  y desde ahí:
  - manda comandos JSON (take_photo, start_recording, stop_recording,
    start_live_view, stop_live_view)
  - recibe eventos JSON (fotos guardadas, wake word detectada, etc.)
  - recibe binarios (frames de la vista en vivo) mientras esté activa
"""
import json
import logging
from fastapi import APIRouter, WebSocket, WebSocketDisconnect
from app.connections import manager
from app.models import DeviceCommand
from app.ws_device import close_recording, start_recording_audio, is_recording

logger = logging.getLogger("mobile_ws")
router = APIRouter()

@router.websocket("/ws/app/{device_id}")
async def mobile_socket(websocket: WebSocket, device_id: str):
    await manager.connect_viewer(device_id, websocket)
    await websocket.send_text(json.dumps({
        "type": "device_status",
        "payload": {"connected": manager.get_device(device_id) is not None},
        "recording": is_recording(device_id),
    }))
    logger.info("App conectada, viendo dispositivo: %s", device_id)

    try:
        while True:
            raw_text = await websocket.receive_text()
            await _handle_command(device_id, raw_text)

    except WebSocketDisconnect:
        logger.info("App desconectada de: %s", device_id)
        manager.disconnect_viewer(device_id, websocket)
        
async def _handle_command(device_id: str, raw_text: str) -> None:
    try:
        cmd = DeviceCommand.model_validate_json(raw_text)
    except Exception:
        logger.warning("Comando inválido de la app: %s", raw_text)
        return

    device_ws = manager.get_device(device_id)
    if device_ws is None:
        logger.warning("Comando %s pero %s no está conectado", cmd.type, device_id)
        return

    if cmd.type == "start_recording":
        start_recording_audio(device_id)

    if cmd.type == "stop_recording":
        close_recording(device_id)

    await device_ws.send_text(json.dumps({"type": cmd.type, "payload": cmd.payload or {}}))
    logger.info("Comando %s -> %s", cmd.type, device_id)