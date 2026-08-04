# Window Manager & Desktop Shell Architecture

Mectov OS includes a graphical windowing environment (`src/gui/wm.c`, `desktop.c`, `taskbar.c`, `login.c`) featuring floating z-ordered windows, ToaruOS-inspired dark window chrome, Aero Snap, and interactive squircle desktop icons. The taskbar keeps the retro SerenityOS/Win95 bevel look for a distinctive '90s-desktop identity.

---

## 🪟 Window Manager (`src/gui/wm.c`)

1. **Window Properties**:
   - Manages floating windows with unique IDs, position (`x, y`), dimensions (`width, height`), title, and active double-buffer pointer.
   - 1px solid frame in the active/inactive border color, drawn inside the window bounds so dirty-rect culling stays exact.

2. **Window Controls (ToaruOS dark glyph buttons)**:
   - Titlebar uses a dark gray vertical gradient (lighter `#585858` → `#3b3b3b`) when focused, darker tones when inactive, with a 16x16 app icon on the left and centered light-gray title text.
   - **Close Button (X)**: Closes window and frees resources (`wm_close`); red rounded hover background.
   - **Minimize Button (bar)**: Hides window to taskbar; gray rounded hover background.
   - **Maximize / Restore Button (square)**: Toggles full-screen geometry; shows an overlapping-squares glyph when maximized.
   - Button geometry is computed once in `wm_btn_geom()` and shared by rendering, hover tracking (`wm_track_mouse`), and hit-testing, so the hit boxes can never drift from what is drawn.
   - The Alt+Tab switcher HUD card uses the same dark chrome with a ToaruOS-blue selection highlight.

3. **Aero Snap & Resizing**:
   - Dragging title bar to screen edges triggers automatic snap (Left half, Right half, or Fullscreen top).
   - Dragging window edges/corners resizes window dynamically with minimum constraints (220x150).

---

## 🖥️ Desktop Shell (`src/gui/desktop.c` & `taskbar.c`)

1. **Wallpaper & Squircle Icons**:
   - Baked 1024x768 32-bit BGRA wallpaper.
   - Rounded squircle desktop icons with minimalist glyphs and curated color palettes for applications (Explorer, Terminal, Doom, Browser, Notepad, Calculator, Flappy, Snake, SysInfo).

2. **Taskbar & System Tray** (retro SerenityOS bevel style):
   - Gray `#c0c0c0` taskbar face with recessed top groove, raised-bevel Start button (sunken when pressed), and Win95 blue selection highlight in the Start Menu.
   - Active task buttons display 16x16 application squircle icons in raised bevels that go sunken when the window is focused.
   - System tray wrapped in a sunken inset panel: CAPS lock state, HDD activity LED, volume, power, and real-time WIB (UTC+7) clock.
   - Shared 3D bevel helpers (`vga_bevel_raised`/`vga_bevel_sunken`) live in `src/drivers/vga.c` and are reused by the taskbar; the shared `draw_app_icon()` helper is reused by the WM titlebar icons.
