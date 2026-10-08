"""Configuración del backend, leída de variables de entorno (.env)."""
from pydantic_settings import BaseSettings, SettingsConfigDict

class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8")
    
    # Server
    host: str = "0.0.0.0"
    port: int = 8000
    
    # Gemini apikey
    gemini_api_key: str = ""
    
    # Carpeta donde se guardaran las fotos y videos
    media_dir: str = "/data/media"
    
settings = Settings()