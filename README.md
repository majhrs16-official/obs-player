# obs-music-playlist

OBS Studio plugin that plays music from a configurable directory with playlist controls and displays the current track name.

## Características

- **Fuente de audio** (`Music Playlist Audio`): Reproducción push-model desde directorio configurable
- **Fuente de texto** (`Music Playlist Text`): Muestra nombre del track actual (sin extensión)
- **Controles de playlist**: Play/Pausa, Stop, Siguiente, Anterior, Reiniciar, Seek
- **Modos de repetición**: Ninguno, Repetir Canción, Repetir Playlist
- **Aleatorio (Shuffle)**: Orden aleatorio de reproducción
- **Controles multimedia nativos**: Integración completa con dock de controles multimedia de OBS
- **Scene switching inteligente**: Pausa/reanuda automáticamente al cambiar escenas sin perder posición

## Core Features (v1.0.0)

- **Push-model audio output**: `obs_source_output_audio()` llamado directamente desde hilo decodificador
- **Estado compartido**: `PlaylistState` singleton único entre audio y texto
- **Escaneo recursivo**: Descubre archivos mp3, flac, ogg, wav, m4a, aac
- **Shuffle Fisher-Yates**: Algoritmo robusto para aleatoriedad
- **Hilo decodificador dedicado**: FFmpeg + swresample → float stereo 48kHz
- **Rate limiting preciso**: Timestamp relativo separado (`rate_limit_timestamp_ns`) que se resetea en seeks/stops

## Media Controls (OBS Native)

El plugin implementa la interfaz multimedia completa de OBS:

- `media_play_pause` - Play/Pausa
- `media_stop` - Stop
- `media_restart` - Reiniciar track actual
- `media_next` - Siguiente track
- `media_previous` - Track anterior
- `media_get_duration` - Duración total (ms)
- `media_get_time` - Posición actual (ms)
- `media_set_time` - Seek a posición (ms)
- `media_get_state` - Estado: Playing / Paused / Stopped

Accesibles desde el dock "Controles multimedia" de OBS seleccionando la fuente de audio.

## Arquitectura

- **PlaylistState singleton**: Gestiona playlist, hilo decodificador, estado de reproducción, posición guardada
- **Audio source (push model)**: Hilo decodificador llama `obs_source_output_audio()` directamente, rate limiting interno
- **Text source (observador)**: Renderiza via FreeType2 → RGBA → gs_texture con manual technique passes (estilo `text_freetype2`)
- **Cross-platform**: Linux (pkg-config) y Windows (find_package) con rutas de fuentes condicionales

## Scene Switching

Al cambiar de escena en OBS:

1. **Deactivate**: Guarda `played_frames` + `audio_timestamp_ns`, pausa decodificador, mantiene source viva
2. **Activate**: Restaura posición, `seek_pending` al timestamp guardado, reanuda reproducción
3. **Sin interrupción**: La música continúa desde el punto exacto al volver a la escena

## Uso

1. Añadir fuente **Music Playlist Audio** a la escena
2. Configurar en propiedades: Directorio de música, Modo repetición, Aleatorio
3. Añadir fuente **Music Playlist Text** (opcional) para mostrar track actual
4. Usar dock **Controles multimedia** de OBS (seleccionar audio source) para control de reproducción

## Propiedades de la Fuente de Audio

- **Directory**: Ruta a archivos de música (escaneo recursivo)
- **Repeat Mode**: None / Repeat Song / Repeat Playlist
- **Shuffle**: Randomizar orden de reproducción

## Fuente de Texto

Sin propiedades - observa automáticamente el track actual de la fuente de audio.

## Limitaciones Conocidas

- **Solo Linux** (build Windows pendiente - CMake listo pero sin CI)
- **Un directorio por instancia** (para múltiples carpetas, usar múltiples instancias del plugin)
- **Sin metadatos ID3** en texto (usa `std::filesystem::path::stem()` del nombre de archivo)
- **Requiere audio monitoring** en OBS para escucha local

## Dependencias

- OBS Studio (libobs) 29.x / 30.x
- FFmpeg: libavformat, libavcodec, libavutil, libswresample
- FreeType2

## Building

### Linux

```bash
mkdir build && cd build
cmake ..
make -j4
make install
```

Instala en `~/.config/obs-studio/plugins/obs-music-playlist/bin/64bit/obs-music-playlist.so`

### Windows

```bash
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

Instala en `%APPDATA%\obs-studio\plugins\obs-music-playlist\bin\64bit\obs-music-playlist.dll`

## Licencia

GPL-3.0 License