# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Basic build
meson setup build/
meson compile -C build/

# Build with ASAN/UBSAN for debugging crashes
meson setup -Db_sanitize=address,undefined build/

# Build with tests enabled
meson setup -Dtest=enabled build/
meson compile -C build/
meson test --verbose -C build/

# Force use of bundled wlroots
meson setup --force-fallback-for=wlroots build/

# Skip wlroots download if not found
meson setup --wrap-mode=nodownload build/

# Install without wlroots headers (when using wlroots.wrap)
meson install --skip-subprojects -C build/
```

Build options: `-Dxwayland=disabled`, `-Dsvg=disabled`, `-Dicon=disabled`, `-Dlabnag=disabled`

## Running and Debugging

```bash
# Run with debug logging
./build/labwc -d 2>debug.log

# Wayland protocol debugging
WAYLAND_DEBUG=1 foot                    # Client-side
WAYLAND_DEBUG=server labwc              # Server-side (run nested)

# Visual damage debugging
WLR_SCENE_DEBUG_DAMAGE=highlight labwc

# Emulate multiple outputs
WLR_WL_OUTPUTS=2 labwc

# Software rendering (rule out GPU issues)
WLR_RENDERER=pixman labwc
```

## Code Style Check

```bash
./scripts/check [--file=<filename>]
```

Uses Linux kernel checkpatch.pl. Key rules:
- Tabs (8 columns), 80-char soft line limit
- Opening braces on same line except function definitions
- Function names on new line: `return_type\nfunction_name(params)`
- Braces mandatory even for single statements
- `#include`: angle brackets first (alphabetized), then quoted (alphabetized)
- Public functions prefixed with filename (e.g., `view_*()` in view.c)
- Handlers use `handle_*` prefix
- Enums prefixed with `LAB_`

## Architecture

labwc is a wlroots-based Wayland compositor. Key structs in `include/labwc.h`:

**`struct server`** - Global compositor state
- `wl_display`, `wlr_backend`, `wlr_renderer` - Core Wayland/wlroots
- `views` - Linked list of windows (front-to-back)
- `seat` - Input/focus management
- `scene` - wlroots scene-graph for rendering
- `workspace_tree`, `xdg_popup_tree`, `menu_tree` - Scene hierarchy
- `workspaces` - Virtual desktop management

**`struct view`** (`include/view.h`) - Window representation
- `impl` - Function pointers for XDG_SHELL or XWAYLAND views
- `pending`, `current` - Window geometry
- `workspace`, `primary_output` - Placement

**`struct seat`** - Input handling
- Cursor, keyboard, touch, tablet management
- Pointer constraints, gestures
- Focus state

**Input modes** (`enum input_mode`):
- `LAB_INPUT_STATE_PASSTHROUGH` - Normal operation
- `LAB_INPUT_STATE_MOVE/RESIZE` - Window manipulation
- `LAB_INPUT_STATE_MENU` - Context menu active
- `LAB_INPUT_STATE_CYCLE` - Alt-tab switching

**Coordinate systems**: `(sx,sy)` surface, `(ox,oy)` output, `(lx,ly)` layout

## Key Source Files

- `src/server.c` - Initialization and protocol registration
- `src/view.c` - Window management
- `src/desktop.c` - Focus, raise, arrange operations
- `src/output.c` - Display rendering
- `src/seat.c` - Input/focus management
- `src/action.c` - Keybind/mousebind actions
- `src/config/rcxml.c` - Configuration parsing
- `src/input/cursor.c` - Pointer handling
- `src/ssd/` - Server-side decorations

## Internal API

Use these helpers from `include/common/`:
- `znew(type)` - Calloc with type checking (mem.h)
- `zfree(ptr)` - Free and zero (mem.h)
- `wl_list_append()` - Add to end of list (list.h)
- `wl_array_len()` - Get array element count (array.h)
- `ARRAY_SIZE()` - Visible array size (macros.h)

## Configuration

User config: `~/.config/labwc/` with `rc.xml`, `menu.xml`, `autostart`, `shutdown`, `environment`, `themerc-override`

Example configs: `docs/rc.xml.all`, `docs/menu.xml`

Man pages: `docs/labwc-config.5.scd`, `docs/labwc-actions.5.scd`, `docs/labwc-theme.5.scd`

## Dependencies

Core: wlroots (>=0.20.0), wayland-server, xkbcommon, libxml2, cairo, pango, glib-2.0, libinput, libpng

Optional: librsvg (SVG buttons), xwayland (X11 support), libsfdo (freedesktop icons)

wlroots source: `subprojects/wlroots.wrap` (auto-downloaded if system version incompatible)
