import { Ionicons } from "@expo/vector-icons";
import { useEffect, useRef, useState } from "react";
import { Alert, AppState, Image, Pressable, StyleSheet, Text, View } from "react-native";

const BACKEND_HOST = process.env.EXPO_PUBLIC_BACKEND_HOST ?? "192.168.1.100";
const DEVICE_ID = "xiao-1";
const WS_URL = `ws://${BACKEND_HOST}:8000/ws/app/${DEVICE_ID}`;
const DEVICE_LABEL = "META Glasses Piratas";

function formatTime(totalSeconds: number) {
  const m = Math.floor(totalSeconds / 60).toString().padStart(2, "0");
  const s = (totalSeconds % 60).toString().padStart(2, "0");
  return `${m}:${s}`;
}

export default function HomeScreen() {
  const [deviceConnected, setDeviceConnected] = useState(false);
  const [liveViewActive, setLiveViewActive] = useState(false);
  const [frameUri, setFrameUri] = useState<string | null>(null);
  const [recording, setRecording] = useState(false);
  const [recordingSeconds, setRecordingSeconds] = useState(0);

  const wsRef = useRef<WebSocket | null>(null);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const frameQueueRef = useRef<string[]>([]);
  const frameIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

    const connectWebSocket = () => {
    const ws = new WebSocket(WS_URL);
    wsRef.current = ws;

    ws.onclose = () => {
      setDeviceConnected(false);
      setLiveViewActive(false);
      setFrameUri(null);
      frameQueueRef.current = [];
    };
    ws.onerror = () => setDeviceConnected(false);
    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === "live_frame") {
          const uri = `data:image/jpeg;base64,${msg.payload.data}`;
          frameQueueRef.current.push(uri);
          if (frameQueueRef.current.length > 3) {
            frameQueueRef.current.splice(0, frameQueueRef.current.length - 3);
          }
        } else if (msg.type === "photo_saved") {
          Alert.alert("Foto guardada", "Se guardó una nueva foto en el backend");
        } else if (msg.type === "device_status") {
          setDeviceConnected(!!msg.payload.connected);
          // Si nos desconectamos y reconectamos mientras el ESP32 seguía
          // grabando, esto recupera el estado para que puedas detenerla.
          setRecording(!!msg.payload.recording);
          if (!msg.payload.connected) {
            setLiveViewActive(false);
            setFrameUri(null);
            frameQueueRef.current = [];
          }
        }
      } catch {
        // mensaje no-JSON, se ignora
      }
    };
  };

  useEffect(() => {
    frameIntervalRef.current = setInterval(() => {
      if (frameQueueRef.current.length > 0) {
        const next = frameQueueRef.current.shift()!;
        setFrameUri(next);
      }
    }, 160);

    connectWebSocket();

    const appStateSub = AppState.addEventListener("change", (nextState) => {
      if (nextState === "active" && wsRef.current?.readyState !== WebSocket.OPEN) {
        connectWebSocket();
      }
    });

    return () => {
      appStateSub.remove();
      wsRef.current?.close();
      if (timerRef.current) clearInterval(timerRef.current);
      if (frameIntervalRef.current) clearInterval(frameIntervalRef.current);
    };
  }, []);

  const sendCommand = (type: string) => {
    const ws = wsRef.current;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type }));
    } else {
      Alert.alert("Sin conexión", "No se pudo enviar el comando");
    }
  };

  const startLiveView = () => {
    sendCommand("start_live_view");
    setLiveViewActive(true);
  };

  const stopLiveView = () => {
    sendCommand("stop_live_view");
    setLiveViewActive(false);
    setFrameUri(null);
    frameQueueRef.current = [];
  };

  const takePhoto = () => sendCommand("take_photo");

  const toggleRecording = () => {
    if (recording) {
      sendCommand("stop_recording");
      setRecording(false);
      setRecordingSeconds(0);
      if (timerRef.current) clearInterval(timerRef.current);
    } else {
      sendCommand("start_recording");
      setRecording(true);
      setRecordingSeconds(0);
      timerRef.current = setInterval(() => setRecordingSeconds((s) => s + 1), 1000);
    }
  };

  return (
    <View style={styles.container}>
      <View style={styles.headerRow}>
        <View style={[styles.dot, { backgroundColor: deviceConnected ? "#2ecc71" : "#e74c3c" }]} />
        <Text style={styles.deviceName}>{DEVICE_LABEL}</Text>
      </View>
      <Text style={styles.deviceStatus}>
        {deviceConnected ? "Dispositivo Conectado" : "Dispositivo Desconectado"}
      </Text>

      <View style={styles.frameBox}>
        {frameUri ? (
          <Image source={{ uri: frameUri }} style={styles.frame} resizeMode="cover" />
        ) : (
          <Text style={styles.placeholder}>Sin transmisión</Text>
        )}

        <View style={styles.liveBadge}>
          <View style={[styles.liveDot, { backgroundColor: liveViewActive ? "#e74c3c" : "#888" }]} />
          <Text style={styles.liveBadgeText}>EN VIVO</Text>
        </View>

        {recording && (
          <View style={styles.recTimer}>
            <Text style={styles.recTimerText}>{formatTime(recordingSeconds)}</Text>
          </View>
        )}
      </View>

      <View style={styles.card}>
        <Text style={styles.cardTitle}>TRANSMISIÓN</Text>
        <View style={styles.row}>
          <Pressable
            style={[styles.pillBase, liveViewActive ? styles.pillInactive : styles.pillActive]}
            onPress={startLiveView}
            disabled={liveViewActive || !deviceConnected}
          >
            <Ionicons name="play" size={16} color={liveViewActive ? "#888" : "#fff"} />
            <Text style={liveViewActive ? styles.pillInactiveText : styles.pillActiveText}>
              Iniciar Vista
            </Text>
          </Pressable>
          <Pressable
            style={[styles.pillBase, !liveViewActive ? styles.pillInactive : styles.pillActive]}
            onPress={stopLiveView}
            disabled={!liveViewActive || !deviceConnected}
          >
            <Ionicons name="stop" size={16} color={!liveViewActive ? "#888" : "#fff"} />
            <Text style={!liveViewActive ? styles.pillInactiveText : styles.pillActiveText}>
              Detener
            </Text>
          </Pressable>
        </View>
      </View>

      <View style={styles.card}>
        <Text style={styles.cardTitle}>CAPTURA</Text>
        <View style={styles.row}>
          <View style={styles.captureItem}>
            <Pressable
              style={[styles.circleWhite, !deviceConnected && styles.disabled]}
              onPress={takePhoto}
              disabled={!deviceConnected}
            >
              <Ionicons name="camera" size={26} color="#000" />
            </Pressable>
            <Text style={styles.captureLabel}>Foto</Text>
          </View>

          <View style={styles.captureItem}>
            <Pressable
              style={[styles.circleRed, recording && styles.circleRecording, !deviceConnected && styles.disabled]}
              onPress={toggleRecording}
              disabled={!deviceConnected}
            >
              <Ionicons name="videocam" size={26} color="#fff" />
            </Pressable>
            <Text style={styles.captureLabel}>Video</Text>
          </View>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: "#0b0b0f", padding: 16, paddingTop: 50 },
  headerRow: { flexDirection: "row", alignItems: "center", gap: 8, paddingTop:40},
  dot: { width: 10, height: 10, borderRadius: 5 },
  deviceName: { color: "#fff", fontSize: 25, fontWeight: "bold" },
  deviceStatus: { color: "#999", fontSize: 13, marginTop: 2, marginBottom: 16 },

  frameBox: {
    width: "100%",
    height: "48%",
    backgroundColor: "#000",
    borderRadius: 16,
    justifyContent: "center",
    alignItems: "center",
    overflow: "hidden",
  },
  frame: { width: "100%", height: "100%" },
  placeholder: { color: "#555" },

  liveBadge: {
    position: "absolute",
    top: 12,
    left: 12,
    flexDirection: "row",
    alignItems: "center",
    gap: 6,
    backgroundColor: "rgba(0,0,0,0.6)",
    paddingHorizontal: 10,
    paddingVertical: 5,
    borderRadius: 20,
  },
  liveDot: { width: 8, height: 8, borderRadius: 4 },
  liveBadgeText: { color: "#fff", fontSize: 12, fontWeight: "600" },

  recTimer: {
    position: "absolute",
    top: 12,
    right: 12,
    backgroundColor: "rgba(200,0,0,0.85)",
    paddingHorizontal: 10,
    paddingVertical: 5,
    borderRadius: 20,
  },
  recTimerText: { color: "#fff", fontSize: 12, fontWeight: "700" },

  card: { backgroundColor: "#16161d", borderRadius: 14, padding: 14, marginTop: 14 },
  cardTitle: { color: "#888", fontSize: 12, fontWeight: "700", letterSpacing: 1, marginBottom: 10 },
  row: { flexDirection: "row", gap: 12, alignItems: "center", justifyContent: "space-between" },

  pillBase: {
    flex: 1,
    flexDirection: "row",
    gap: 6,
    paddingVertical: 12,
    borderRadius: 10,
    justifyContent: "center",
    alignItems: "center",
  },
  pillActive: { backgroundColor: "#0368cd" },
  pillInactive: { backgroundColor: "#242430" },
  pillActiveText: { color: "#fff", fontWeight: "600" },
  pillInactiveText: { color: "#888", fontWeight: "600" },

  captureItem: { flex: 1, alignItems: "center", gap: 6 },
  circleWhite: { width: 60, height: 60, borderRadius: 30, backgroundColor: "#fff", justifyContent: "center", alignItems: "center" },
  circleRed: { width: 60, height: 60, borderRadius: 30, backgroundColor: "#e74c3c", justifyContent: "center", alignItems: "center" },
  circleRecording: { backgroundColor: "#ff2d2d" },
  disabled: { opacity: 0.35 },
  captureLabel: { color: "#ccc", fontSize: 12 },
});