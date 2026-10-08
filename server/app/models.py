"""Esquemas de los mensajes de control (JSON) que viajan por WebSocket."""
from typing import Any, Literal, Optional
from pydantic import BaseModel


"""Mensaje JSON de control"""
class ControlMessage(BaseModel):
    type: str
    payload: Optional[dict[str, Any]] = None
    
"""Comando que la app o backend manda al ESP32"""
class DeviceCommand(BaseModel):
    type: Literal[
        "take_photo",
        "start_recording",
        "stop_recording",
        "start_live_view",
        "stop_live_view",
        "set_wakeword",
    ]
    payload: Optional[dict[str, Any]] = None