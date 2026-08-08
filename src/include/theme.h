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
#define RETRO_FACE      0x0016130F  // Widget face (now charcoal, was gray)
#define RETRO_HILIGHT   0x003C2E18  // Raised edge highlight (dim amber-brown)
#define RETRO_SHADOW    0x000B0A08  // Raised edge shadow (deep charcoal)
#define RETRO_DKSHADOW  0x002C2821  // Hard outline (IC_LINE)
#define RETRO_TEXT      0x00EDE6D9  // Text on face (IC_INK, was black)
#define RETRO_SEL       0x00E0A94F  // Selection (IC_AMBER, was blue)
#define RETRO_SELTXT    0x0016130F  // Text on selection (dark on amber)
#define RETRO_FACE_LT   0x00DFDFDF  // Lighter face (gradient top)

// Win95-style titlebars (classic blue gradient when focused)
#define RETRO_TITLE_TOP   0x000A246A  // Active titlebar top (navy)
#define RETRO_TITLE_BOT   0x00A6CAF0  // Active titlebar bottom (light blue)
#define RETRO_TITLE_ITOP  0x00C0C0C0  // Inactive titlebar top (gray)
#define RETRO_TITLE_IBOT  0x00A0A0A0  // Inactive titlebar bottom (darker gray)

// ---- ToaruOS-inspired chrome, re-skinned to the Instrument Console ----
// Flat dark titlebars, but in the login screen's warm charcoal + phosphor
// amber: the active titlebar carries an amber hairline + amber glyphs so the
// whole desktop coheres with the gate. (Was neutral gray.)
#define TOARU_TITLE       0x0016130F  // Active titlebar   (IC_BG_PANEL charcoal)
#define TOARU_TITLE_I     0x000B0A08  // Inactive titlebar (IC_BG_DEEP)
#define TOARU_TEXT        0x00EDE6D9  // Active title text  (IC_INK)
#define TOARU_TEXT_I      0x008A8172  // Inactive title text (IC_DIM)
#define TOARU_BORDER      0x00E0A94F  // Active window border (IC_AMBER hairline)
#define TOARU_BORDER_I    0x002C2821  // Inactive window border (IC_LINE)
#define TOARU_BTN_HOV     0x002C2821  // Titlebar button hover (IC_LINE)
#define TOARU_BTN_GLYPH   0x00E0A94F  // Titlebar button glyph (IC_AMBER)

// ---- Taskbar, re-skinned to the Instrument Console ----
// Warm charcoal surface + ink text + phosphor-amber selection, matching the
// login palette. (Was classic gray with black text and blue selection.)
#define TB_BG        0x0016130F  // Taskbar background (IC_BG_PANEL charcoal)
#define TB_BORDER    0x002C2821  // 1px borders / hairline (IC_LINE)
#define TB_BTN       0x0016130F  // Normal task button face (charcoal)
#define TB_BTN_HOV   0x002C2821  // Hovered button (IC_LINE)
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