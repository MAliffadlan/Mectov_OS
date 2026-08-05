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
#define GUI_TASKBAR  0x00181825  // Taskbar background (surface0)
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
// Classic 1990s 3D beveled chrome: light-gray face with white highlight /
// dark shadow edges that make widgets look physically raised or pressed in.
#define RETRO_FACE      0x00C0C0C0  // Widget face (classic gray)
#define RETRO_HILIGHT   0x00FFFFFF  // Raised edge highlight (top/left)
#define RETRO_SHADOW    0x00808080  // Raised edge shadow (bottom/right)
#define RETRO_DKSHADOW  0x00000000  // Hard outline (black)
#define RETRO_TEXT      0x00000000  // Text on gray face
#define RETRO_SEL       0x00000080  // Selection blue (Win95 highlight)
#define RETRO_SELTXT    0x00FFFFFF  // Text on selection
#define RETRO_FACE_LT   0x00DFDFDF  // Lighter face (gradient top)

// Win95-style titlebars (classic blue gradient when focused)
#define RETRO_TITLE_TOP   0x000A246A  // Active titlebar top (navy)
#define RETRO_TITLE_BOT   0x00A6CAF0  // Active titlebar bottom (light blue)
#define RETRO_TITLE_ITOP  0x00C0C0C0  // Inactive titlebar top (gray)
#define RETRO_TITLE_IBOT  0x00A0A0A0  // Inactive titlebar bottom (darker gray)

// ---- ToaruOS-inspired chrome (dark compositor, decor-fancy theme) ----
// Flat dark titlebars (no gradient), centered light title, simple flat glyph
// buttons on the right, thin solid border, square corners.
#define TOARU_TITLE       0x003B3B3B  // Active titlebar   RGB(59, 59, 59)
#define TOARU_TITLE_I     0x001E1E1E  // Inactive titlebar RGB(30, 30, 30)
#define TOARU_TEXT        0x00E6E6E6  // Active title text RGB(230,230,230)
#define TOARU_TEXT_I      0x008C8C8C  // Inactive title text RGB(140,140,140)
#define TOARU_BORDER      0x003B3B3B  // Active window border (thin 1px)
#define TOARU_BORDER_I    0x001E1E1E  // Inactive window border (thin 1px)
#define TOARU_BTN_HOV     0x00555555  // Titlebar button hover (subtle flat)
#define TOARU_BTN_GLYPH   0x00E6E6E6  // Titlebar button glyph

// ---- Classic taskbar (ReactOS / Windows 2000 style) ----
#define TB_BG        0x00C0C0C0  // Taskbar background      RGB(192,192,192)
#define TB_BORDER    0x00808080  // 1px borders / shadow    dark gray
#define TB_BTN       0x00C0C0C0  // Normal task button face (gray)
#define TB_BTN_HOV   0x00D0D0D0  // Hovered button (slightly lighter)
#define TB_BTN_ACT   0x00000080  // Selection blue (start menu / pressed)
#define TB_TEXT      0x00000000  // Primary text             black
#define TB_TEXT_DIM  0x00808080  // Secondary text           dark gray
#define TB_ACTIVE    0x00000080  // Selection / today circle (classic blue)

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