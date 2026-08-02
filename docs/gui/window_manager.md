# Window Manager & Desktop Shell Architecture

Mectov OS includes a modern graphical windowing environment (`src/gui/wm.c`, `desktop.c`, `taskbar.c`, `login.c`) featuring floating z-ordered windows, macOS-style window controls, Aero Snap, and interactive squircle desktop icons.

---

## 🪟 Window Manager (`src/gui/wm.c`)

1. **Window Properties**:
   - Manages floating windows with unique IDs, position (`x, y`), dimensions (`width, height`), title, and active double-buffer pointer.
   - Rounded corner rendering (8px radius) with clean flat borders.

2. **Window Controls ("Traffic Lights")**:
   - **Close Button (Red)**: Closes window and frees resources (`wm_close`).
   - **Minimize Button (Yellow)**: Hides window to taskbar.
   - **Maximize / Restore Button (Green)**: Toggles full-screen geometry.

3. **Aero Snap & Resizing**:
   - Dragging title bar to screen edges triggers automatic snap (Left half, Right half, or Fullscreen top).
   - Dragging window edges/corners resizes window dynamically with minimum constraints (220x150).

---

## 🖥️ Desktop Shell (`src/gui/desktop.c` & `taskbar.c`)

1. **Wallpaper & Squircle Icons**:
   - Baked 1024x768 32-bit BGRA wallpaper.
   - Rounded squircle desktop icons with minimalist glyphs and curated color palettes for applications (Explorer, Terminal, Doom, Browser, Notepad, Calculator, Flappy, Snake, SysInfo).

2. **Taskbar & System Tray**:
   - Start Menu button with glossy Catppuccin Mocha theme accent border.
   - Active task buttons displaying 16x16 application squircle icons.
   - System tray indicators: WiFi status arc icon, CAPS lock state, HDD activity LED, and real-time WIB (UTC+7) clock.
