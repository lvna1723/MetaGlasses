/* Paso 3 del firmware: conectarse al backend por WebSocket y tomar
 * fotos SOLO cuando el backend (o sea, la app) lo pida -- ya no cada
 * 5 segundos. Todavía sin micrófono/wake word -- eso viene después.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "cJSON.h"

#include "camera_pins.h"
#include "wifi_credentials.h"
#include "backend_config.h"

static const char *TAG = "esp32_ai_camera";

/* Debe coincidir con app/protocol.py (BinaryType) del backend. */
#define BIN_TYPE_PHOTO 1

/* --- WiFi: conexión en modo estación (cliente) --- */

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi desconectado, reintentando...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

     /* Como el ESP32 corre con alimentacion USB (no bateria), desactivamos
     * el modo de ahorro de energia del WiFi -- si no, el radio se duerme
     * un instante entre paquetes, lo que causa micro-congelamientos
     * justo como los que se ven en la vista en vivo. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Conectando a %s...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "Señal WiFi (RSSI): %d dBm (canal %d)", ap_info.rssi, ap_info.primary);
    }

    ESP_LOGI(TAG, "WiFi conectado.");
}

/* --- Cámara --- */

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 20,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_hmirror(s, 1); /* 1 = espejo horizontal activado, 0 = desactivado */
    }

    return ESP_OK;
}

/* --- Bocina: MAX98357A por I2S ---
 * Cableado real (según lo soldado):
 *   LRC/WS  -> D0 (GPIO1)
 *   BCLK    -> D1 (GPIO2)
 *   DIN     -> D2 (GPIO3)
 *   GAIN    -> GND  (ganancia fija de 12dB)
 *   SD      -> sin conectar (la mayoría de las placas MAX98357A traen
 *              un pull-up propio que la deja encendida por default;
 *              si no suena nada, revisa que tu placa lo tenga o
 *              conecta SD a 3V3).
 */
#define I2S_BCLK_GPIO   GPIO_NUM_2
#define I2S_WS_GPIO     GPIO_NUM_1
#define I2S_DOUT_GPIO   GPIO_NUM_3
#define I2S_SAMPLE_RATE 16000

static i2s_chan_handle_t s_tx_chan;

static void audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

/* Genera y manda un tono simple por la bocina. Se usa tanto para el
 * tono de prueba al arrancar como para el "beep" de confirmación
 * cuando se detecta la wake word. */
static void play_beep(float freq_hz, float duration_s)
{
    const int total_samples = (int)(I2S_SAMPLE_RATE * duration_s);
    const int chunk_samples = 256;
    const float amplitude = 6000.0f; /* de 32767 max -- volumen moderado */

    int16_t buf[chunk_samples];
    int samples_done = 0;
    uint32_t phase = 0;

    while (samples_done < total_samples) {
        int n = (total_samples - samples_done < chunk_samples) ? (total_samples - samples_done) : chunk_samples;
        for (int i = 0; i < n; i++) {
            float t = (float)phase / (float)I2S_SAMPLE_RATE;
            buf[i] = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * freq_hz * t));
            phase++;
        }
        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, buf, n * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        samples_done += n;
    }
}

/* Tono de prueba al arrancar (nota "La", 440Hz) -- solo para
 * confirmar que la bocina realmente suena. */
static void play_test_tone(void)
{
    ESP_LOGI(TAG, "Reproduciendo tono de prueba...");
    play_beep(440.0f, 1.0f);
    ESP_LOGI(TAG, "Tono terminado.");
}

/* --- Micrófono PDM integrado del XIAO Sense ---
 * GPIO42 = CLK, GPIO41 = DATA (fijo en la placa, no es cableado
 * nuestro). Usamos I2S_NUM_1 (distinto al de la bocina, I2S_NUM_0)
 * para poder grabar y reproducir al mismo tiempo. */
#define I2S_MIC_CLK_GPIO GPIO_NUM_42
#define I2S_MIC_DIN_GPIO GPIO_NUM_41

static i2s_chan_handle_t s_rx_chan;

static void mic_init(void)
{
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_MIC_CLK_GPIO,
            .din = I2S_MIC_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
}

/* Graba del micrófono y lo manda derechito a la bocina, en tiempo
 * real, por unos segundos -- para confirmar que el micrófono
 * funciona sin meter al backend/Gemini todavía. */
static void mic_loopback_test(void)
{
    const int seconds = 20;
    const int chunk_samples = 256;
    int16_t buf[chunk_samples];
    const int total_chunks = (I2S_SAMPLE_RATE * seconds) / chunk_samples;
    const int chunks_per_log = I2S_SAMPLE_RATE / chunk_samples;
    const float gain = 20.0f; /* la señal sin el nivel base suele venir muy bajita */

    ESP_LOGI(TAG, "Prueba de microfono: habla o aplaude cerca del microfono (%d s)...", seconds);

    int16_t running_max = 0;
    int32_t dc_estimate = 0;

    for (int i = 0; i < total_chunks; i++) {
        size_t bytes_read = 0, bytes_written = 0;
        i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, portMAX_DELAY);

        int n = bytes_read / sizeof(int16_t);
        for (int j = 0; j < n; j++) {
            /* Filtro simple para quitar el nivel base (DC) de la señal */
            dc_estimate += ((int32_t)buf[j] - dc_estimate) >> 6;
            int32_t sample = (int32_t)buf[j] - dc_estimate;
            sample = (int32_t)(sample * gain);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            buf[j] = (int16_t)sample;

            int16_t v = buf[j];
            if (v < 0) v = -v;
            if (v > running_max) running_max = v;
        }
        if (i % chunks_per_log == 0) {
            ESP_LOGI(TAG, "  nivel de audio (tras quitar base y subir volumen): %d", running_max);
            running_max = 0;
        }

        i2s_channel_write(s_tx_chan, buf, bytes_read, &bytes_written, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "Prueba de microfono terminada.");
}

/* --- Wake word: "Jarvis", usando WakeNet (ESP-SR) ---
 * Tarea que corre para siempre, escuchando el micrófono en bloques
 * pequeños (los que pida WakeNet) y avisando cuando detecta la
 * palabra de activación. No bloquea el resto del programa: WiFi,
 * cámara y el WebSocket siguen su curso en paralelo. */
static void ws_send_text(const char *msg);
static void ws_send_mic_chunk(const int16_t *pcm, int n_samples);
static void ws_send_recording_audio_chunk(const int16_t *pcm, int n_samples);
static volatile bool s_recording_active = false;

static void wakeword_task(void *arg)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "wn9_jarvis_tts");
    if (model_name == NULL) {
        ESP_LOGE(TAG, "No se encontro el modelo de wake word 'wn9_jarvis_tts' (revisa que este habilitado en menuconfig)");
        vTaskDelete(NULL);
        return;
    }
    esp_wn_iface_t *wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    model_iface_data_t *model_data = wakenet->create(model_name, DET_MODE_90);
    int chunk_samples = wakenet->get_samp_chunksize(model_data);
    int16_t *buffer = malloc(chunk_samples * sizeof(int16_t));
    int32_t dc_estimate = 0;
    const float gain = 14.0f;
    const int chunk_ms = (chunk_samples * 1000) / I2S_SAMPLE_RATE;

    /* Umbral de energia para decidir si hay voz o silencio mientras se
     * graba, sobre el promedio de |muestra| del chunk (misma senal ya
     * preprocesada con DC-removal + ganancia que usamos para el wake
     * word). Si corta antes de que termines de hablar, sube este
     * numero; si tarda mucho en cortar, bajalo. Mira los logs de
     * "nivel de audio" para ver que valores da tu voz vs el silencio. */
    const int32_t VOICE_ENERGY_THRESHOLD = 450;
    const int SILENCE_HANG_MS = 1200;
    const int MAX_WAIT_FOR_SPEECH_MS = 4000;
    const int MAX_RECORDING_MS = 15000;

    ESP_LOGI(TAG, "Escuchando la wake word 'Jarvis'... (bloques de %d muestras)", chunk_samples);

    while (1) {
        size_t bytes_read = 0;
        i2s_channel_read(s_rx_chan, buffer, chunk_samples * sizeof(int16_t), &bytes_read, portMAX_DELAY);

        int n = bytes_read / sizeof(int16_t);
        for (int i = 0; i < n; i++) {
            dc_estimate += ((int32_t)buffer[i] - dc_estimate) >> 6;
            int32_t sample = ((int32_t)buffer[i] - dc_estimate) * gain;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            buffer[i] = (int16_t)sample;
        }

        if (s_recording_active) {
            ws_send_recording_audio_chunk(buffer, n);
        }

        wakenet_state_t state = wakenet->detect(model_data, buffer);
        if (state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "¡Jarvis detectado! Grabando...");
            play_beep(880.0f, 0.15f);
            ws_send_text("{\"type\":\"wake_word_detected\"}");
            ws_send_text("{\"type\":\"mic_audio_start\"}");

            bool speech_started = false;
            int silence_ms = 0;
            int elapsed_ms = 0;
            int log_counter = 0;

            while (1) {
                size_t r_bytes = 0;
                i2s_channel_read(s_rx_chan, buffer, chunk_samples * sizeof(int16_t), &r_bytes, portMAX_DELAY);
                int rn = r_bytes / sizeof(int16_t);

                int64_t sum_abs = 0;
                for (int i = 0; i < rn; i++) {
                    dc_estimate += ((int32_t)buffer[i] - dc_estimate) >> 6;
                    int32_t sample = ((int32_t)buffer[i] - dc_estimate) * gain;
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    buffer[i] = (int16_t)sample;
                    sum_abs += (sample < 0) ? -sample : sample;
                }
                int32_t avg_abs = (rn > 0) ? (int32_t)(sum_abs / rn) : 0;

                ws_send_mic_chunk(buffer, rn);

                if (s_recording_active) {
                    ws_send_recording_audio_chunk(buffer, rn);
                }

                elapsed_ms += chunk_ms;
                if (++log_counter >= 10) {
                    log_counter = 0;
                    ESP_LOGI(TAG, "nivel de audio (grabando): %ld%s", (long)avg_abs, speech_started ? " [hablando]" : " [esperando]");
                }

                if (avg_abs > VOICE_ENERGY_THRESHOLD) {
                    speech_started = true;
                    silence_ms = 0;
                } else if (speech_started) {
                    silence_ms += chunk_ms;
                }

                if (speech_started && silence_ms >= SILENCE_HANG_MS) {
                    ESP_LOGI(TAG, "Silencio detectado, fin de la grabacion.");
                    break;
                }
                if (!speech_started && elapsed_ms >= MAX_WAIT_FOR_SPEECH_MS) {
                    ESP_LOGW(TAG, "No se detecto voz, cancelando grabacion.");
                    break;
                }
                if (elapsed_ms >= MAX_RECORDING_MS) {
                    ESP_LOGW(TAG, "Tope de tiempo de grabacion alcanzado.");
                    break;
                }
            }

            ws_send_text("{\"type\":\"mic_audio_end\"}");
        }
    }

    /* No debería llegar aquí, pero por limpieza: */
    wakenet->destroy(model_data);
    free(buffer);
    vTaskDelete(NULL);
}

/* --- WebSocket al backend --- */
static esp_websocket_client_handle_t s_ws_client;

#define BIN_TYPE_MIC_AUDIO 4
#define BIN_TYPE_RECORDING_AUDIO 6

static void ws_send_text(const char *msg)
{
    if (s_ws_client) {
        esp_websocket_client_send_text(s_ws_client, msg, strlen(msg), portMAX_DELAY);
    }
}

static void ws_send_mic_chunk(const int16_t *pcm, int n_samples)
{
    if (!s_ws_client) return;
    size_t payload_len = n_samples * sizeof(int16_t);
    size_t out_len = payload_len + 1;
    uint8_t *out = malloc(out_len);
    if (!out) return;
    out[0] = BIN_TYPE_MIC_AUDIO;
    memcpy(out + 1, pcm, payload_len);
    esp_websocket_client_send_bin(s_ws_client, (const char *)out, out_len, portMAX_DELAY);
    free(out);
}

static void ws_send_recording_audio_chunk(const int16_t *pcm, int n_samples)
{
    if (!s_ws_client) return;
    size_t payload_len = n_samples * sizeof(int16_t);
    size_t out_len = payload_len + 1;
    uint8_t *out = malloc(out_len);
    if (!out) return;
    out[0] = BIN_TYPE_RECORDING_AUDIO;
    memcpy(out + 1, pcm, payload_len);
    esp_websocket_client_send_bin(s_ws_client, (const char *)out, out_len, portMAX_DELAY);
    free(out);
}

/* Comandos que el backend puede mandarnos (ver app/models.py DeviceCommand) */
#define BIN_TYPE_TTS_AUDIO 5

/* Reproduce un pedazo de audio de respuesta (PCM16 mono a 16kHz, mismo
 * formato que usa la bocina) que nos manda el backend despues de hablar
 * con Gemini. */
static void handle_tts_audio_chunk(const uint8_t *data, size_t len)
{
    if (len < 1 || data[0] != BIN_TYPE_TTS_AUDIO) {
        return;
    }
    const int16_t *pcm_in = (const int16_t *)(data + 1);
    int n_samples = (len - 1) / sizeof(int16_t);
    const float gain = 0.6f;

    static int16_t out[2048];  // suficiente para el tamaño de chunk que manda el backend (1024 muestras)
    if (n_samples > 2048) {
        n_samples = 2048; // por seguridad, nunca nos salimos del arreglo
    }

    for (int i = 0; i < n_samples; i++) {
        int32_t sample = (int32_t)(pcm_in[i] * gain);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out[i] = (int16_t)sample;
    }

    size_t bytes_written = 0;
    i2s_channel_write(s_tx_chan, out, n_samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    ESP_LOGI(TAG, "TTS: %d muestras recibidas, %zu bytes escritos al I2S", n_samples, bytes_written);
}

static uint8_t s_tts_frame_buf[4096];
static size_t s_tts_frame_received = 0;

/* El audio TTS casi siempre llega partido en 2 o más pedazos del
 * transporte WebSocket (aunque logicamente sea "un solo mensaje" de
 * ~2KB). Los vamos juntando en este buffer usando payload_offset,
 * hasta completar payload_len, y ahi si lo mandamos a reproducir. */
static void handle_tts_ws_frame(const uint8_t *data_ptr, int data_len, int payload_offset, int payload_len)
{
    if (payload_offset == 0) {
        s_tts_frame_received = 0;
    }

    if (payload_offset + data_len > (int)sizeof(s_tts_frame_buf)) {
        ESP_LOGE(TAG, "Frame TTS demasiado grande para el buffer (%d bytes)", payload_offset + data_len);
        s_tts_frame_received = 0;
        return;
    }

    memcpy(s_tts_frame_buf + payload_offset, data_ptr, data_len);
    s_tts_frame_received += data_len;

    if (s_tts_frame_received >= (size_t)payload_len) {
        handle_tts_audio_chunk(s_tts_frame_buf, payload_len);
        s_tts_frame_received = 0;
    }
}

typedef enum {
    CMD_UNKNOWN = 0,
    CMD_TAKE_PHOTO,
    CMD_START_RECORDING,
    CMD_STOP_RECORDING,
    CMD_START_LIVE_VIEW,
    CMD_STOP_LIVE_VIEW,
    CMD_SET_WAKEWORD,
} cmd_type_t;

static cmd_type_t parse_command_type(const char *json_str, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) {
        return CMD_UNKNOWN;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    cmd_type_t result = CMD_UNKNOWN;

    if (cJSON_IsString(type_item) && type_item->valuestring != NULL) {
        const char *t = type_item->valuestring;
        if (strcmp(t, "take_photo") == 0) result = CMD_TAKE_PHOTO;
        else if (strcmp(t, "start_recording") == 0) result = CMD_START_RECORDING;
        else if (strcmp(t, "stop_recording") == 0) result = CMD_STOP_RECORDING;
        else if (strcmp(t, "start_live_view") == 0) result = CMD_START_LIVE_VIEW;
        else if (strcmp(t, "stop_live_view") == 0) result = CMD_STOP_LIVE_VIEW;
        else if (strcmp(t, "set_wakeword") == 0) result = CMD_SET_WAKEWORD;
    }

    cJSON_Delete(root);
    return result;
}

static void handle_take_photo(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "No se pudo capturar la foto");
        return;
    }

    /* El backend espera [1 byte de tipo][bytes de la foto], ver
     * app/protocol.py -> unpack(). */
    size_t out_len = fb->len + 1;
    uint8_t *out = malloc(out_len);
    if (out) {
        out[0] = BIN_TYPE_PHOTO;
        memcpy(out + 1, fb->buf, fb->len);
        esp_websocket_client_send_bin(s_ws_client, (const char *)out, out_len, portMAX_DELAY);
        ESP_LOGI(TAG, "Foto enviada: %zu bytes", fb->len);
        free(out);
    } else {
        ESP_LOGE(TAG, "Sin memoria para armar el paquete de la foto");
    }

    esp_camera_fb_return(fb);
}

/* --- Vista en vivo: captura y manda frames en bucle mientras esté activa --- */

#define BIN_TYPE_LIVE_FRAME 3

static volatile bool s_live_view_active = false;
static TaskHandle_t s_live_view_task_handle = NULL;

static void live_view_task(void *arg)
{
    ESP_LOGI(TAG, "Vista en vivo iniciada");
    while (s_live_view_active) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            size_t out_len = fb->len + 1;
            uint8_t *out = malloc(out_len);
            if (out) {
                out[0] = BIN_TYPE_LIVE_FRAME;
                memcpy(out + 1, fb->buf, fb->len);

                int64_t t0 = esp_timer_get_time();
                esp_websocket_client_send_bin(s_ws_client, (const char *)out, out_len, portMAX_DELAY);
                int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

                if (elapsed_ms > 100) {
                    ESP_LOGW(TAG, "Frame de %zu bytes tardo %lld ms en mandarse", fb->len, elapsed_ms);
                }
                free(out);
            }
            esp_camera_fb_return(fb);
        }
        /* ~6-7 fps: de sobra para una vista en vivo y liviano para el WiFi */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGI(TAG, "Vista en vivo detenida");
    s_live_view_task_handle = NULL;
    vTaskDelete(NULL);
}

static void handle_start_live_view(void)
{
    if (s_live_view_active) {
        return; /* ya está corriendo, no hacemos nada */
    }
    s_live_view_active = true;
    xTaskCreate(live_view_task, "live_view", 4096, NULL, 5, &s_live_view_task_handle);
}

static void handle_stop_live_view(void)
{
    /* La tarea revisa esta bandera en cada vuelta y se autodestruye. */
    s_live_view_active = false;
}
/* --- Grabación de video: igual que la vista en vivo, pero con su
 * propio tipo de binario para que el backend sepa que estos frames
 * hay que guardarlos (y no solo mostrarlos como la vista en vivo). */

#define BIN_TYPE_RECORDING_FRAME 2   /* el siguiente libre: ya usas 1,3,4,5 */

static TaskHandle_t s_recording_task_handle = NULL;

static void recording_task(void *arg)
{
    ESP_LOGI(TAG, "Grabacion iniciada");
    while (s_recording_active) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            size_t out_len = fb->len + 1;
            uint8_t *out = malloc(out_len);
            if (out) {
                out[0] = BIN_TYPE_RECORDING_FRAME;
                memcpy(out + 1, fb->buf, fb->len);
                esp_websocket_client_send_bin(s_ws_client, (const char *)out, out_len, portMAX_DELAY);
                free(out);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(150)); /* mismo ritmo que la vista en vivo; ajústalo si quieres más/menos fps */
    }
    ESP_LOGI(TAG, "Grabacion detenida");
    s_recording_task_handle = NULL;
    vTaskDelete(NULL);
}

static void handle_start_recording(void)
{
    if (s_recording_active) {
        return; /* ya está grabando */
    }
    s_recording_active = true;
    xTaskCreate(recording_task, "recording", 4096, NULL, 5, &s_recording_task_handle);
}

static void handle_stop_recording(void)
{
    s_recording_active = false;
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado al backend");
        {
            const char *hello = "{\"type\":\"hello\",\"payload\":{\"fw\":\"0.3\"}}";
            esp_websocket_client_send_text(s_ws_client, hello, strlen(hello), portMAX_DELAY);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Desconectado del backend, el cliente reintentará solo");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->payload_offset == 0 &&
            data->data_len == data->payload_len) {
            cmd_type_t cmd = parse_command_type(data->data_ptr, data->data_len);
            switch (cmd) {
            case CMD_TAKE_PHOTO:
                handle_take_photo();
                break;
            case CMD_START_LIVE_VIEW:
                handle_start_live_view();
                break;
            case CMD_STOP_LIVE_VIEW:
                handle_stop_live_view();
                break;
            case CMD_START_RECORDING:
                handle_start_recording();
                break;
            case CMD_STOP_RECORDING:
                handle_stop_recording();
                break;
            case CMD_SET_WAKEWORD:
                ESP_LOGW(TAG, "Comando reconocido pero aún no implementado (%d)", cmd);
                break;
            default:
                ESP_LOGW(TAG, "Comando desconocido: %.*s", data->data_len, data->data_ptr);
            }
        } else if (data->op_code == 0x02) {
            /* Audio de respuesta (TTS) -- casi siempre llega fragmentado,
             * lo reensamblamos con payload_offset/payload_len. */
            handle_tts_ws_frame((const uint8_t *)data->data_ptr, data->data_len,
                                 data->payload_offset, data->payload_len);
        }
        /* op_code 0x09/0x0A son ping/pong internos del cliente websocket,
         * no hay nada que hacer con ellos aquí. */
        break;

    default:
        ESP_LOGD(TAG, "Evento de websocket no manejado: %d", (int)event_id);
        break;
    }
}

static void websocket_start(void)
{
     esp_websocket_client_config_t ws_cfg = {
        .uri = BACKEND_WS_URI,
        .buffer_size = 4096,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 3000,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Prueba rápida de la bocina, antes que nada más -- así confirmamos
     * el hardware de audio sin depender de que WiFi/cámara funcionen. */
    audio_init();
    play_test_tone();

    mic_init();
    /* Ya confirmamos que el micrófono funciona con mic_loopback_test();
     * ahora en vez de eso arrancamos la escucha de la wake word, que
     * corre en su propia tarea para siempre. */
    xTaskCreate(wakeword_task, "wakeword", 8192, NULL, 5, NULL);

    wifi_init_sta();

    ESP_LOGI(TAG, "Inicializando cámara...");
    esp_err_t err = camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al inicializar la cámara: 0x%x", err);
        return;
    }
    ESP_LOGI(TAG, "Cámara lista.");

    ESP_LOGI(TAG, "Conectando al backend: %s", BACKEND_WS_URI);
    websocket_start();

    /* Todo lo demás pasa por eventos (WiFi, WebSocket); aquí no hay
     * nada más que hacer en el loop principal. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
