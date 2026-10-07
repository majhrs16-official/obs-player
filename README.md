# obs-music-playlist

OBS Studio plugin that plays music from a configurable directory with playlist controls and displays the current track name.

## Features

- **Audio Source** (`Music Playlist Audio`): Plays music files from a directory
- **Text Source** (`Music Playlist Text`): Displays current track name (without extension)
- **Playlist Controls**: Play/Pause, Stop, Next, Previous, Restart
- **Repeat Modes**: None, Repeat Song, Repeat Playlist
- **Shuffle**: Randomize playlist order
- **Media Controls Integration**: Works with OBS media controls dock

## Architecture

- Single shared `PlaylistState` singleton manages playlist, decoder thread, and playback state
- Audio source uses push model (`obs_source_output_audio`) from decoder thread
- Text source observes shared state and renders via FreeType → RGBA → gs_texture
- Cross-platform: Linux and Windows

## Dependencies

- OBS Studio (libobs)
- FFmpeg (libavformat, libavcodec, libavutil, libswresample)
- FreeType2

## Building

### Linux

```bash
mkdir build && cd build
cmake ..
make -j4
make install
```

Installs to `~/.config/obs-studio/plugins/obs-music-playlist/bin/64bit/obs-music-playlist.so`

### Windows

```bash
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

Installs to `%APPDATA%\obs-studio\plugins\obs-music-playlist\bin\64bit\obs-music-playlist.dll`

## Usage

1. Add **Music Playlist Audio** source to your scene
2. Configure directory path, repeat mode, shuffle in source properties
3. Add **Music Playlist Text** source to display current track name
4. Use OBS media controls dock (select audio source) for playback control

## Source Properties (Audio Source)

- **Directory**: Path to music files
- **Repeat Mode**: None / Repeat Song / Repeat Playlist
- **Shuffle**: Randomize playback order

## Text Source

No properties - automatically observes the audio source's current track.

## License

MIT License