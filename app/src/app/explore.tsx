import { Directory, File, Paths } from "expo-file-system";
import * as MediaLibrary from "expo-media-library";
import { useFocusEffect } from "expo-router";
import { useVideoPlayer, VideoView } from "expo-video";
import { useCallback, useState } from "react";
import {
  Alert,
  FlatList,
  Image,
  Modal,
  Pressable,
  RefreshControl,
  StyleSheet,
  Text,
  View,
} from "react-native";

const BACKEND_HOST = process.env.EXPO_PUBLIC_BACKEND_HOST ?? "192.168.1.100";
const DEVICE_ID = "xiao-1";
const API_BASE = `http://${BACKEND_HOST}:8000`;

type MediaItem = {
  filename: string;
  kind: "photo" | "video";
  url: string;
  size_bytes: number;
};

function VideoPlayerView({ uri }: { uri: string }) {
  const player = useVideoPlayer(uri, (p) => {
    p.play();
  });
  return (
    <VideoView
      style={styles.fullImage}
      player={player}
      allowsPictureInPicture
      surfaceType="textureView"
    />
  );
}

export default function GalleryScreen() {
  const [items, setItems] = useState<MediaItem[]>([]);
  const [refreshing, setRefreshing] = useState(false);
  const [selected, setSelected] = useState<MediaItem | null>(null);

  const load = useCallback(async () => {
    setRefreshing(true);
    try {
      const res = await fetch(`${API_BASE}/media/${DEVICE_ID}`);
      const data = await res.json();
      setItems(data.items ?? []);
    } catch {
      Alert.alert("Error", "No se pudo cargar la galería. ¿Está corriendo el backend?");
    } finally {
      setRefreshing(false);
    }
  }, []);

  useFocusEffect(
    useCallback(() => {
      load();
    }, [load])
  );

  const saveToPhone = async (item: MediaItem) => {
    const { status } = await MediaLibrary.requestPermissionsAsync();
    if (status !== "granted") {
      Alert.alert("Permiso necesario", "Necesito permiso para guardar en tu galería");
      return;
    }
    try {
      const dest = new Directory(Paths.cache, "descargas");
      if (!dest.exists) dest.create({ intermediates: true });
      const file = await File.downloadFileAsync(`${API_BASE}${item.url}`, dest);
      await MediaLibrary.saveToLibraryAsync(file.uri);
      Alert.alert("Listo", "Se guardó en la galería de tu celular");
    } catch {
      Alert.alert("Error", "No se pudo guardar el archivo");
    }
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Galería</Text>
      <FlatList
        data={items}
        numColumns={3}
        keyExtractor={(item) => item.filename}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={load} />}
        renderItem={({ item }) =>
          item.kind === "photo" ? (
            <Pressable onPress={() => setSelected(item)} style={styles.thumbWrap}>
              <Image source={{ uri: `${API_BASE}${item.url}` }} style={styles.thumb} />
            </Pressable>
          ) : (
            <Pressable onPress={() => setSelected(item)} style={[styles.thumbWrap, styles.videoThumb]}>
              <Text style={styles.videoIcon}>▶️</Text>
            </Pressable>
          )
        }
        ListEmptyComponent={
          <Text style={styles.placeholder}>Aún no hay fotos ni videos guardados</Text>
        }
      />

      <Modal visible={!!selected} transparent animationType="fade">
        <View style={styles.modalBg}>
          {selected && (
            <>
              {selected.kind === "photo" ? (
                <Image
                  source={{ uri: `${API_BASE}${selected.url}` }}
                  style={styles.fullImage}
                  resizeMode="contain"
                />
              ) : (
                <VideoPlayerView key={selected.url} uri={`${API_BASE}${selected.url}`} />
              )}
              <View style={styles.modalButtons}>
                <Pressable style={styles.modalBtn} onPress={() => saveToPhone(selected)}>
                  <Text style={styles.modalBtnText}>Guardar en mi celular</Text>
                </Pressable>
                <Pressable style={styles.modalBtn} onPress={() => setSelected(null)}>
                  <Text style={styles.modalBtnText}>Cerrar</Text>
                </Pressable>
              </View>
            </>
          )}
        </View>
      </Modal>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, padding: 8 },
  title: { color: "#fff", fontSize: 45, fontWeight: "bold", marginBottom: 8, marginTop: 80 },
  thumbWrap: { flex: 1 / 3, aspectRatio: 1, padding: 2 },
  thumb: { width: "100%", height: "100%", borderRadius: 4 },
  videoLabel: { fontSize: 10, textAlign: "center" },
  placeholder: { textAlign: "center", marginTop: 40, color: "#888" },
  modalBg: {
    flex: 1,
    backgroundColor: "rgba(0,0,0,0.9)",
    justifyContent: "center",
    alignItems: "center",
  },
  fullImage: { width: "100%", height: "80%" },
  modalButtons: { flexDirection: "row", gap: 16, marginTop: 16 },
  modalBtn: { backgroundColor: "#333", paddingVertical: 10, paddingHorizontal: 16, borderRadius: 8 },
  modalBtnText: { color: "white" },
  videoThumb: { backgroundColor: "#1a1a22", justifyContent: "center", alignItems: "center" },
  videoIcon: { fontSize: 24 },
});