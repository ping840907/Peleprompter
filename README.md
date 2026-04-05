# Peleprompter

A teleprompter application for **Pebble smartwatches** with an **Android companion app**. The watch displays scrolling text while the phone manages content, bookmarks, and remote control.

## Project Structure

```
peleprompter-project/
├── pebble-watchapp/             # Pebble C watchapp
│   ├── package.json             # Pebble project config (UUID, keys, platforms)
│   ├── wscript                  # Pebble build script
│   └── src/
│       ├── c/
│       │   ├── constants.h      # Shared constants, protocol keys, buffer sizes
│       │   ├── ring_buffer.h/c  # Ring buffer (doubly-linked chunk list)
│       │   ├── messaging.h/c    # AppMessage communication layer
│       │   ├── settings_window.h/c  # In-watch settings (speed, text size)
│       │   └── main.c           # Main app: rendering, scrolling, button logic
│       └── pkjs/
│           └── index.js         # PebbleKit JS bridge (minimal)
│
└── android-companion/           # Android Kotlin companion app
    ├── build.gradle.kts         # Top-level Gradle config
    ├── settings.gradle.kts
    ├── gradle.properties
    └── app/
        ├── build.gradle.kts     # App-level dependencies (PebbleKit, Material, Gson)
        ├── proguard-rules.pro
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/com/peleprompter/
            │   ├── PeleprompterApp.kt           # Application class
            │   ├── model/
            │   │   └── TextDocument.kt          # Document + settings models
            │   ├── service/
            │   │   └── PebbleCommManager.kt     # PebbleKit communication
            │   ├── ui/
            │   │   ├── MainActivity.kt          # Main UI controller
            │   │   ├── MainViewModel.kt         # ViewModel (MVVM)
            │   │   └── HistoryAdapter.kt        # Import history list
            │   └── util/
            │       ├── PebbleProtocol.kt        # Protocol constants
            │       └── PrefsManager.kt          # SharedPreferences persistence
            └── res/
                ├── layout/
                │   ├── activity_main.xml        # Main layout
                │   └── item_history.xml         # History list item
                ├── drawable/
                │   └── edit_text_bg.xml         # EditText background
                ├── values/
                │   ├── strings.xml
                │   └── themes.xml               # Dark Material theme
                └── xml/
                    └── file_paths.xml           # FileProvider paths
```

## Architecture

### Pebble Watch: Ring Buffer Text Streaming

The watch has strict memory limits, so text is streamed in chunks from the phone:

```
┌──────────────────────────────────────────────────┐
│                   RING BUFFER                     │
│  ┌─────────┐   ┌─────────┐   ┌─────────┐        │
│  │ Chunk A  │──▶│ Chunk B  │──▶│ Chunk C  │       │
│  │ offset=0 │◀──│ offset=  │◀──│ offset=  │       │
│  │          │   │ 512      │   │ 1024     │       │
│  └─────────┘   └─────────┘   └─────────┘        │
│      HEAD                         TAIL            │
│                                                   │
│  ◄─── prepend (scroll up)    append (scroll down)─►│
│  ◄─── evict tail             evict head ──────────►│
└──────────────────────────────────────────────────┘
```

**Key behaviors:**
- **Prefetching**: When the viewport approaches the buffer boundary (within N screen heights), the watch requests the next chunk from the phone
- **Bidirectional**: Scrolling up triggers prepend-fetching; scrolling down triggers append-fetching
- **Memory eviction**: When the buffer exceeds capacity, oldest chunks (opposite end from scroll direction) are evicted
- **Scroll offset adjustment**: When prepending chunks, the pixel scroll offset is adjusted by the height delta to prevent visual jumping
- **Buffer underrun**: If the user scrolls faster than chunks arrive, a "Loading..." indicator appears and auto-scroll pauses until new data arrives

### Communication Protocol

```
Watch ──[CMD_REQUEST_TEXT, offset]──▶ Phone
Watch ◀──[CMD_SEND_TEXT, offset, chunk]── Phone
Watch ◀──▶──[CMD_SYNC_SETTINGS, speed, size]── Phone
```

| Key | ID | Type | Description |
|-----|-----|------|-------------|
| KEY_COMMAND | 0 | Int | 0=ReqText, 1=SendText, 2=SyncSettings |
| KEY_TEXT_OFFSET | 1 | Int | Character offset in source text |
| KEY_TEXT_CHUNK | 2 | String | Text payload (preserves `\n`) |
| KEY_SCROLL_SPEED | 3 | Int | Speed level 1-6 |
| KEY_TEXT_SIZE | 4 | Int | 0=Small, 1=Medium, 2=Large |

### Offline Grace Period

When Bluetooth disconnects, the watch does **not** immediately alert the user. They can continue reading the buffered text. The "Disconnected" warning only appears when the user scrolls past the end of the loaded buffer.

### Watch Controls

| Button | Short Press | Long Press |
|--------|------------|------------|
| UP | Scroll up 20px | Scroll up 80px |
| DOWN | Scroll down 20px | Scroll down 80px |
| SELECT | Play/Pause auto-scroll | Open Settings |

**Manual scroll + auto-scroll interaction:** Manual scrolling pauses auto-scroll for 2 seconds, then auto-scroll resumes automatically.

### Platform Adaptations

| Platform | Buffer Size | Chunk Size | Round Insets |
|----------|------------|------------|-------------|
| Aplite | 3KB | 256B | N/A |
| Basalt, Diorite, Emery | 8KB | 512B | N/A |
| Chalk, Gabbro | 6KB | 512B | 18px H, 10px V |

## Pebble App UUID

```
1ced8e88-c6d6-476d-8f55-dc51edf6d9a7
```

## Building

### Pebble Watchapp

```bash
cd pebble-watchapp
pebble build
pebble install --emulator basalt   # or --phone <IP>
```

Requires the Pebble SDK (rebble toolchain).

### Android Companion

Open `android-companion/` in Android Studio and build normally. Requires:
- Android SDK 34
- Kotlin 1.9+
- Gradle 8.4+

The PebbleKit dependency (`com.getpebble:pebblekit:4.0.1`) may need a custom repository if the original Maven artifact is unavailable. In that case, include the PebbleKit AAR manually in `app/libs/`.

## Android App Features

- **Paste text** or **import .txt files**
- **Import history** with tap-to-reload and swipe-to-delete
- **Bookmark tracking**: Saves reading progress; prompts "Start from Beginning" vs "Resume from Bookmark"
- **Remote control**: Adjust watch scroll speed (1-6) and text size (S/M/L) from the phone
- **Dark Material theme** UI

## License

MIT
