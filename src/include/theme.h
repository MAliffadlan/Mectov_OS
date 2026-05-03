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

// Icon backgrounds
#define GUI_ICON_BG  0x00222233

// Text
#define GUI_TEXT     0x00CDD6F4  // Primary text (text)
#define GUI_TEXT_INV 0x00BAC2DE  // Text on dark (subtext0)
#define GUI_DIM      0x006C7086  // Dim/subtle text (overlay2)
#define GUI_WHITE    0x00FFFFFF

// ---- Layout constants ----
#define TITLEBAR_H   20          // Was 24 — more compact like toaruOS
#define TASKBAR_H_PX 32          // Was 28 — more room for taskbar buttons
#define WIN_RADIUS   8           // Window corner radius
#define BTN_RADIUS   5           // Titlebar button radius (small circles)
#define SHADOW_LAYERS 4          // Number of drop shadow layers

// ---- Modern feature flags ----
#define WINDOW_SNAP_THRESHOLD  20  // Pixels from edge to trigger snap
#define TASKBAR_BTN_MAX_W     160  // Max width for taskbar window buttons
#define TASKBAR_BTN_MIN_W      80  // Min width for taskbar window buttons

#endif