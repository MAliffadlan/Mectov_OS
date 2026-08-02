# VESA VBE Display Driver & Graphics Engine

Mectov OS features a high-performance 32-bit linear framebuffer graphics engine (`src/drivers/vga.c`) operating in VESA VBE mode at 1024x768 resolution with 32-bit BGRA color depth.

---

## 🎨 Framebuffer Architecture

1. **VBE Multiboot Initialization**:
   - GRUB Multiboot initializes VBE graphics mode `1024x768x32`.
   - `kernel_main` receives physical framebuffer base address (`fb_p`) and size (`fb_s`).
   - `paging_init()` identity-maps the linear video memory range.

2. **Triple-Buffered Shadow Rendering Pipeline**:
   - **Back Buffer (`back_buffer`)**: Off-screen memory where windows, desktop icons, taskbar, and wallpaper are drawn.
   - **Front Buffer Copy (`front_buffer_copy`)**: Mirror copy of the previously displayed frame.
   - **Hardware VBE MMIO (`fb_p`)**: Physical video memory mapped to graphics card.
   - **Delta Copying**: During `full_redraw()`, the engine compares `back_buffer` with `front_buffer_copy` and writes ONLY modified 32-bit pixel blocks to hardware video memory. This eliminates thousands of expensive KVM VM-Exits per frame.

---

## 🖱️ Hardware Mouse Cursor Engine

1. **High-Definition 16x24 Bitmap**:
   - Sleek arrow cursor with inner dark fill and crisp white border.
   - **Dynamic Drop Shadow**: Computes real-time 50% alpha-blended shadow cast 3 pixels down and right.

2. **Dirty Region Software Cursor**:
   - Restores background pixels under cursor before redrawing new cursor position.
   - Zero screen tearing or mouse flicker during rapid movement.
