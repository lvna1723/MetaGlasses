import logging
from fastapi import FastAPI
from app import ws_device, ws_mobile, media_api


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

app = FastAPI(title="ESP32 AI CAM BACKEND")

app.include_router(ws_device.router)
app.include_router(ws_mobile.router)
app.include_router(media_api.router)

@app.get("/health")
def health():
    return {"status": "ok"}