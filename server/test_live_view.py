"""Prueba de la vista en vivo: se conecta como si fuera la app, manda
start_live_view, y muestra cuántos frames van llegando del ESP32.

Requiere: el backend corriendo y el ESP32 ya conectado (revisa el
monitor serial -> "Conectado al backend").

Detén con Ctrl+C -- al hacerlo, también manda stop_live_view para que
el ESP32 deje de mandar frames.
"""
import asyncio
import json
import websockets

DEVICE_ID = "xiao-1"  # debe coincidir con backend_config.h del firmware


async def main():
    uri = f"ws://127.0.0.1:8000/ws/app/{DEVICE_ID}"
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "start_live_view"}))
        print("start_live_view enviado, esperando frames... (Ctrl+C para detener)")
        count = 0
        try:
            while True:
                raw = await ws.recv()
                data = json.loads(raw)
                if data.get("type") == "live_frame":
                    count += 1
                    size = len(data["payload"]["data"])
                    print(f"Frame #{count} recibido (~{size} bytes en base64)")
                else:
                    print("Otro evento:", data)
        except KeyboardInterrupt:
            pass
        finally:
            await ws.send(json.dumps({"type": "stop_live_view"}))
            print("stop_live_view enviado.")


asyncio.run(main())
