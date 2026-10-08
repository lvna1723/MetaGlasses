"""Prueba manual: simula el ESP32 y la app hablando con el backend."""
import asyncio
import json

import websockets

DEVICE_ID = "esp32-test"


async def fake_device():
    async with websockets.connect(f"ws://127.0.0.1:8000/ws/cam/{DEVICE_ID}") as ws:
        await ws.send(json.dumps({"type": "hello", "payload": {"fw": "0.1"}}))
        # esperamos el comando take_photo de la app
        cmd_raw = await asyncio.wait_for(ws.recv(), timeout=5)
        cmd = json.loads(cmd_raw)
        print("[device] recibí comando:", cmd)
        assert cmd["type"] == "take_photo"
        # "tomamos una foto" (bytes falsos) y la mandamos con el tipo binario 1 = PHOTO
        fake_jpeg = b"\xff\xd8\xff\xe0FAKEJPEGDATA"
        await ws.send(bytes([1]) + fake_jpeg)
        await asyncio.sleep(1)


async def fake_app():
    async with websockets.connect(f"ws://127.0.0.1:8000/ws/app/{DEVICE_ID}") as ws:
        await asyncio.sleep(0.5)  # dejar que el device se conecte primero
        await ws.send(json.dumps({"type": "take_photo"}))
        print("[app] mandé take_photo")
        event_raw = await asyncio.wait_for(ws.recv(), timeout=5)
        print("[app] recibí evento:", event_raw)
        assert json.loads(event_raw)["type"] == "photo_saved"


async def main():
    await asyncio.gather(fake_device(), fake_app())
    print("\nOK: el flujo completo (app -> backend -> device -> backend -> app) funcionó.")


asyncio.run(main())
