# test_device_only.py
import asyncio
import websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:8000/ws/cam/esp32-test") as ws:
        await ws.send('{"type": "hello", "payload": {}}')
        print("Conectado, mandé hello. Esperando 3s...")
        await asyncio.sleep(3)
    print("Cerrado sin problemas.")

asyncio.run(main())