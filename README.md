# Peleprompter

A teleprompter application for **Pebble smartwatches** with an **Android companion app**. The phone renders text into 1-bit bitmap pages and streams them to the watch; the watch displays a smoothly scrolling image at configurable speed and text size.

## Project Structure

```
peleprompter/
├── pebble-watchapp/
│   ├── package.json                 # App UUID, platform targets, resource declarations
│   ├── resources/images/
│   │   └── menu_icon.png            # 25×25 app menu icon (3-bar teleprompter motif)
│   └── src/
│       ├── c/
│       │   ├── constants.h          # Protocol keys, commands, text size enum, persist keys
│       │   ├── image_manager.h/c    # GBitmap page cache (up to MAX_PAGES_CACHED pages)
│       │   ├── messaging.h/c        # AppMessage send/receive, ACK-driven queue
│       │   ├── settings_window.h/c  # In-watch settings menu (speed, text size, time bar)
│       │   └── main.c               # Scrolling canvas, button logic, status bar, init
│       └── pkjs/
│           └── index.js             # PebbleKit JS: config page HTML + image rendering
│
└── android-companion/
    └── app/src/main/
        ├── AndroidManifest.xml
        ├── java/com/peleprompter/
        │   ├── PeleprompterApp.kt
        │   ├── model/
        │   │   └── TextDocument.kt          # TextDocument, WatchSettings, ImportHistoryEntry
        │   ├── service/
        │   │   ├── PebbleCommManager.kt     # PebbleKit communication + page dispatch
        │   │   └── WatchImageRenderer.kt    # Text → 1-bit bitmap renderer
        │   ├── ui/
        │   │   ├── MainActivity.kt          # Main screen controller
        │   │   ├── MainViewModel.kt         # MVVM ViewModel, settings migration
        │   │   ├── HistoryAdapter.kt        # Import history RecyclerView
        │   │   ├── WatchEmulatorActivity.kt # On-device watch emulator screen
        │   │   ├── WatchSimulator.kt        # Simulates watch ↔ phone protocol
        │   │   ├── WatchScreenView.kt       # Custom view renders 1-bit bitmap
        │   │   └── LogAdapter.kt            # Protocol log RecyclerView
        │   └── util/
        │       ├── PebbleProtocol.kt        # Protocol constants (mirrors constants.h)
        │       └── PrefsManager.kt          # SharedPreferences persistence
        └── res/
            ├── layout/
            │   ├── activity_main.xml        # Main screen layout
            │   ├── activity_watch_emulator.xml
            │   ├── item_history.xml
            │   └── item_log_entry.xml
            └── drawable/
                ├── ic_launcher_background.xml   # Purple (#BB86FC) adaptive icon background
                ├── ic_launcher_foreground.xml   # 3-bar icon foreground
                ├── source_badge_empty.xml        # Script source badge (no script)
                └── source_badge_loaded.xml       # Script source badge (script loaded)
```

## Architecture

### Image-Based Rendering

The phone renders the entire script into 1-bit (black/white) bitmap pages — one page per watch screen — before transmission begins. Each page is encoded in **GBitmapFormat1Bit** layout (MSB = leftmost pixel, 1 = white, row-stride = ⌈width/8⌉ bytes).

```
Phone                                    Watch
──────                                   ─────
[WatchImageRenderer]                     [ImageManager]
  Text → StaticLayout                      page cache (≤ MAX_PAGES_CACHED)
  → 1-bit bitmap pages                     ↓
  → base64 chunks ─────CMD_INIT_IMAGES──▶  totalPages received
                        CMD_REQUEST_PAGE◀─  watch requests page N
                   ─CMD_SEND_IMAGE_CHUNK──▶  watch stores GBitmap
                                          [main.c canvas]
                                            draws page(s) at scroll offset
```

**Scroll model:** `s_scroll_offset_px` is a global virtual position across the full document height (`totalPages × pageHeight`). Each frame, the canvas draws whichever page(s) cover the current viewport — potentially two pages at a page boundary — by slicing the GBitmap with sub-rects.

**Prefetch:** When the scroll offset approaches the next page boundary within `IMAGE_PREFETCH_THRESHOLD_PX` (≈ 2 screen heights), the watch pre-requests that page so it arrives before it scrolls into view.

### Communication Protocol

| Constant | ID | Direction | Payload |
|----------|----|-----------|---------|
| `CMD_REQUEST_TEXT` | 0 | Watch → Phone | Screen width/height, text size level |
| `CMD_SYNC_SETTINGS` | 2 | Both | Scroll speed, text size level |
| `CMD_INIT_IMAGES` | 3 | Phone → Watch | Total pages, render width/height, start page |
| `CMD_REQUEST_PAGE` | 4 | Watch → Phone | Page number |
| `CMD_SEND_IMAGE_CHUNK` | 5 | Phone → Watch | Chunk index, total chunks, raw bytes |

| Key | ID | Description |
|-----|----|-------------|
| `KEY_COMMAND` | 0 | Command type (see above) |
| `KEY_SCROLL_SPEED` | 3 | Auto-scroll speed 1–6 |
| `KEY_TEXT_SIZE` | 4 | Text size level 0–4 |
| `KEY_PAGE_NUM` | 5 | Page index (0-based) |
| `KEY_CHUNK_INDEX` | 6 | Chunk index within a page |
| `KEY_TOTAL_CHUNKS` | 7 | Total chunks for this page |
| `KEY_IMAGE_DATA` | 8 | Raw bitmap bytes for this chunk |
| `KEY_TOTAL_PAGES` | 9 | Total rendered pages |
| `KEY_WATCH_WIDTH` | 10 | Watch screen width (px) |
| `KEY_WATCH_HEIGHT` | 11 | Watch screen height (px) |
| `KEY_START_PAGE` | 12 | Initial page to display (0-based), for bookmark resume |

Messages are sent through an **ACK-driven queue**: the next message is only dispatched after the previous one is acknowledged, preventing watch inbox overflow.

### Text Size Levels

Five levels are supported. Levels 0 and 1 use a light typeface for thinner strokes on the 1-bit display. The pixel sizes, line spacing (×1.15) and typefaces are **unified across `index.js` and Android** so both renderers paginate identically.

| Level | Name | Size (px) | Typeface |
|-------|------|-----------|---------|
| 0 | Tiny | 10 | light (`sans-serif-light` / weight 300) |
| 1 | Small | 13 | light (`sans-serif-light` / weight 300) |
| 2 | Medium | 16 | `MONOSPACE` |
| 3 | Large | 22 | `MONOSPACE` |
| 4 | XLarge | 28 | `MONOSPACE` |

> **Migration note:** Saved settings from the previous 3-level system (0 = Small, 1 = Medium, 2 = Large) are automatically shifted +2 on first launch so visual size is preserved.

### Platform Support

| Platform | Screen | Round insets |
|----------|--------|-------------|
| Aplite | 144 × 168 | — |
| Basalt / Diorite / Flint | 144 × 168 | — |
| Chalk | 180 × 180 | 20 px H |
| Emery | 200 × 228 | — |
| Gabbro | 260 × 260 | 30 px H |

### Watch Controls

| Button | Tap | Hold |
|--------|-----|------|
| UP | Scroll up 60 px | Continuous scroll up (60 px every 100 ms) |
| DOWN | Scroll down 60 px | Continuous scroll down (60 px every 100 ms) |
| SELECT | Play / Pause auto-scroll | Open Settings menu |

Scrolling **up** pauses auto-scroll for 2 seconds, then resumes automatically. Scrolling **down** does not interrupt auto-scroll.

### In-Watch Settings Menu

Accessed via SELECT long-press. Changes are persisted across power cycles.

- **Scroll Speed** — levels 1–6 (65 ms/px at level 3 default, 18 ms/px at level 6)
- **Text Size** — Tiny / Small / Medium / Large / XLarge
- **Display → Time Bar** — toggle PebbleOS status bar (16 px strip at top)

### Status Bar (Time Bar)

When enabled, a `StatusBarLayer` occupies the top 16 px. The canvas and status-text layer are offset accordingly so content never overlaps the time display.

## Android Companion App

### Script Input

The main screen has a unified **Script** card:

- **Paste** — type or paste text directly into the editor; tap **Use Pasted Text** to load it
- **Import .txt File** — pick a file from device storage
- A persistent **source badge** shows the current script's origin (📋 pasted / 📄 file name), character count, and a clear (✕) button
- **Send to Watch** card shows push controls only after a script is loaded

### Remote Control

- **Scroll Speed** slider (1–6)
- **Text Size** toggle: Tiny / Small / Medium / Large / XLarge
- Changes are pushed to the watch immediately via `CMD_SYNC_SETTINGS`

### Import History

Previous imports are listed with title, date, and a preview. Tap to reload; swipe to delete.

### Watch Emulator

The **Emulator** button opens `WatchEmulatorActivity`, which simulates the full watch ↔ phone protocol on-device:

- Select platform and text size
- Scroll controls mirror the physical watch buttons
- A live protocol log shows every message exchanged

## Configuration Page (PebbleKit JS)

Accessible from the Pebble mobile app's settings gear. The config page is built entirely in `index.js` (`buildConfigHtml()`) — no external server required.

**Features:**
- Paste script text
- Select platform (Aplite/Basalt · Chalk · Emery · Gabbro)
- Select text size (Tiny → XLarge)
- **Render & Connect** — renders all pages client-side using an HTML `<canvas>`, encodes them as 1-bit bitmaps, and passes the result back to PebbleKit JS

## Building

### Pebble Watchapp

Requires the Pebble/Rebble SDK toolchain.

```bash
cd pebble-watchapp
pebble build
pebble install --emulator basalt   # or: --phone <IP>
```

### Android Companion

Open `android-companion/` in Android Studio. Requirements:

- Android SDK 34
- Kotlin 1.9+
- Gradle 8.4+

The PebbleKit dependency (`com.getpebble:pebblekit:4.0.1`) requires a Rebble-hosted Maven repository. If unavailable, include the PebbleKit AAR manually in `app/libs/`.

## License

MIT
