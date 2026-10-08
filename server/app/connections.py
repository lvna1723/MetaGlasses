"""Registro en memoria de quién está conectado por WebSocket.

Con un solo dispositivo y una sola app usándolo, esto es suficiente.
Mapea device_id -> conexión del ESP32, y device_id -> lista de apps
viendo ese dispositivo (para poder relayar la vista en vivo a varias
pantallas si algún día hace falta).
"""
from starlette.websockets import WebSocket


class ConnectionManager:
    def __init__(self) -> None:
        self.devices: dict[str, WebSocket] = {}
        self.viewers: dict[str, set[WebSocket]] = {}
        
    # ESP32
    async def connect_device(self, device_id: str, ws: WebSocket) -> None:
        old_ws = self.devices.get(device_id)
        await ws.accept()
        self.devices[device_id] = ws
        if old_ws is not None and old_ws is not ws:
            # El ESP32 se reconecto (reflasheo, reset, etc.) sin que la
            # conexion vieja se cerrara -- la cerramos para no dejarla
            # huerfana (eso causaba el "semaphore timeout" en Windows).
            try:
                await old_ws.close()
            except Exception:
                pass
        
    def disconnect_device(self, device_id: str) -> None:
        self.devices.pop(device_id, None)
        
    def get_device(self, device_id: str) -> WebSocket | None:
        return self.devices.get(device_id)
    
    # App movil
    async def connect_viewer(self, device_id: str, ws: WebSocket) -> None:
        await ws.accept()
        self.viewers.setdefault(device_id, set()).add(ws)
        
    def disconnect_viewer(self, device_id: str, ws: WebSocket) -> None:
        if device_id in self.viewers:
            self.viewers[device_id].discard(ws)
            
    async def broadcast_to_viewers(self, device_id: str, data: bytes) -> None:
        for ws in list(self.viewers.get(device_id, [])):
            try: 
                await ws.send_bytes(data)
            except Exception:
                self.disconnect_viewer(device_id, ws)


manager = ConnectionManager()