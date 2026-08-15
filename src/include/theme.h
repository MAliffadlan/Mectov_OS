#ifndef THEME_H
#define THEME_H

// ---- GUI Theme Colors (Catppuccin Mocha-inspired modern dark) ----

// Base palette
#define C_BLACK      0x00000000
#define C_WHITE      0x00FFFFFF
#define C_GREEN      0x0000FF00
#define C_CYAN       0x0000FFFF
#define C_BLUE       0x000000FF
#define C_NAVY       0x00000080
#define C_GRAY       0x00808080
#define C_DARK_GRAY  0x00404040
#define C_YELLOW     0x00FFFF00

// ---- Modern Dark Theme (Catppuccin Mocha refined) ----
// Backgrounds
#define GUI_BG       0x001E1E2E  // Window body background (mantle)
#define GUI_DESKTOP  0x0011111B  // Desktop background (crust)
#define GUI_TASKBAR  0x00000000  // Taskbar background (black; fallback strip in d_desktop)
#define GUI_TASKBAR_A 0xCC181825 // Taskbar semi-transparent (was 0x88)

// Borders
#define GUI_BORDER   0x00585870  // Active window border (overlay0)
#define GUI_BORDER2  0x00313144  // Inactive window border (surface1)

// Titlebars
#define GUI_TITLE_A  0x00313244  // Active titlebar top (surface1)
#define GUI_TITLE_B  0x0045475A  // Active titlebar bottom (surface2)
#define GUI_TITLE_I  0x001E1E2E  // Inactive titlebar (mantle)

// Buttons
#define GUI_BTN      0x00313244  // Normal button
#define GUI_BTN_HOV  0x0045475A  // Hovered button

// Accent colors
#define GUI_BLUE     0x0089B4FA  // Soft blue accent
#define GUI_GREEN    0x0027C93F  // Vibrant macOS Green
#define GUI_YELLOW   0x00FFBD2E  // Vibrant macOS Yellow
#define GUI_CLOSE    0x00FF5F56  // Vibrant macOS Red
#define GUI_MAGENTA  0x00CBA6F7  // Soft purple
#define GUI_TEAL     0x0094E2D5  // Soft teal
#define GUI_RED      0x00FF5F56  // Alias for close
#define GUI_ORANGE   0x00FAB387  // Peach accent

// ---- Retro (SerenityOS-inspired) bevel palette ----
// Classic 1990s 3D beveled chrome: dark face with subtle light highlight /
// dark shadow edges that make widgets look physically raised or pressed in.
// v38.37: the face and bevel edges are now pure black / neutral gray (they
// used to be warm charcoal + amber-brown, which read as "orange" on the
// taskbar). These constants are only consumed by the taskbar bevels.
#define RETRO_FACE      0x00000000  // Widget face (pure black)
#define RETRO_HILIGHT   0x00282828  // Raised edge highlight (neutral gray)
#define RETRO_SHADOW    0x000E0E0E  // Raised edge shadow (neutral deep)
#define RETRO_DKSHADOW  0x00303030  // Hard outline (neutral gray)
#define RETRO_TEXT      0x00EDE6D9  // Text on face (IC_INK, was black)
#define RETRO_SEL       0x00E0A94F  // Selection (IC_AMBER, was blue)
#define RETRO_SELTXT    0x00000000  // Text on selection (black on amber)
#define RETRO_FACE_LT   0x00DFDFDF  // Lighter face (gradient top)

// Win95-style titlebars (classic blue gradient when focused)
#define RETRO_TITLE_TOP   0x000A246A  // Active titlebar top (navy)
#define RETRO_TITLE_BOT   0x00A6CAF0  // Active titlebar bottom (light blue)
#define RETRO_TITLE_ITOP  0x00C0C0C0  // Inactive titlebar top (gray)
#define RETRO_TITLE_IBOT  0x00A0A0A0  // Inactive titlebar bottom (darker gray)

// ---- ToaruOS-inspired chrome, re-skinned to the Instrument Console ----
// v38.37: full-black window chrome — no amber hairline, no amber glyphs
// (the "yellow line" around focused windows). Titlebars and borders are
// pure black; the window-control glyphs are off-white so they stay visible
// on the black bar.
#define TOARU_TITLE       0x00000000  // Active titlebar   (black)
#define TOARU_TITLE_I     0x00000000  // Inactive titlebar (black)
#define TOARU_TEXT        0x00EDE6D9  // Active title text  (IC_INK)
#define TOARU_TEXT_I      0x008A8172  // Inactive title text (IC_DIM)
#define TOARU_BORDER      0x00000000  // Active window border (black)
#define TOARU_BORDER_I    0x00000000  // Inactive window border (black)
#define TOARU_BTN_HOV     0x00282828  // Titlebar button hover (neutral gray)
#define TOARU_BTN_GLYPH   0x00EDE6D9  // Titlebar button glyph (off-white)

// ---- Taskbar, re-skinned to the Instrument Console ----
// v38.37: full-black surface (no warm/orange tint) + ink text; the amber
// accents stay only as selection feedback in popups.
#define TB_BG        0x00000000  // Taskbar background (pure black)
#define TB_BORDER    0x00303030  // 1px borders / hairline (neutral gray)
#define TB_BTN       0x00000000  // Normal task button face (black)
#define TB_BTN_HOV   0x00282828  // Hovered button (neutral gray)
#define TB_BTN_ACT   0x00E0A94F  // Selection (pressed / active, IC_AMBER)
#define TB_TEXT      0x00EDE6D9  // Primary text (IC_INK)
#define TB_TEXT_DIM  0x008A8172  // Secondary text (IC_DIM)
#define TB_ACTIVE    0x00E0A94F  // Selection / today circle (IC_AMBER)

// Icon backgrounds
#define GUI_ICON_BG  0x00222233

// Text
#define GUI_TEXT     0x00CDD6F4  // Primary text (text)
#define GUI_TEXT_INV 0x00BAC2DE  // Text on dark (subtext0)
#define GUI_DIM      0x006C7086  // Dim/subtle text (overlay2)
#define GUI_WHITE    0x00FFFFFF

// ---- Layout constants ----
#define TITLEBAR_H   20          // Was 24 — more compact like toaruOS
#define TASKBAR_H_PX 28          // Classic ReactOS/Windows 2000 height
#define WIN_RADIUS   8           // Window corner radius
#define BTN_RADIUS   5           // Titlebar button radius (small circles)
#define SHADOW_LAYERS 4          // Number of drop shadow layers

// ---- Modern feature flags ----
#define WINDOW_SNAP_THRESHOLD  20  // Pixels from edge to trigger snap
#define TASKBAR_BTN_W         180  // Fixed task button width (classic)

#endif