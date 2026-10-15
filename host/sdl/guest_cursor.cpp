// guest_cursor.cpp - see guest_cursor.h.
#include "guest_cursor.h"

#include <SDL3/SDL.h>

#include <map>

namespace {

std::map<uint32_t, SDL_Cursor *> g_system; // IDC_* -> SDL system cursor
std::map<uint32_t, SDL_Cursor *> g_colour; // guest handle -> colour cursor
bool g_applied = false;
uint32_t g_applied_handle = 0;
bool g_applied_chosen = false;

SDL_Cursor *cursor_for(uint32_t handle) {
    HostCursorShape shape;
    if (!host_guest_cursor_shape(handle, &shape))
        return SDL_GetDefaultCursor();
    if (shape.stock) {
        SDL_Cursor *&c = g_system[shape.stock];
        if (!c)
            c = SDL_CreateSystemCursor(SDL_SystemCursor(sdl_system_cursor_for_idc(shape.stock)));
        return c ? c : SDL_GetDefaultCursor();
    }
    SDL_Cursor *&c = g_colour[handle];
    if (!c && shape.width > 0 && shape.height > 0) {
        SDL_Surface *surface =
            SDL_CreateSurfaceFrom(shape.width, shape.height, SDL_PIXELFORMAT_ARGB8888,
                                  shape.pixels.data(), shape.width * 4);
        if (surface) {
            c = SDL_CreateColorCursor(surface, shape.hot_x, shape.hot_y);
            SDL_DestroySurface(surface);
        }
    }
    return c ? c : SDL_GetDefaultCursor();
}

} // namespace

int sdl_system_cursor_for_idc(uint32_t idc) {
    switch (idc) {
    case 32513: // IDC_IBEAM
        return SDL_SYSTEM_CURSOR_TEXT;
    case 32514: // IDC_WAIT
        return SDL_SYSTEM_CURSOR_WAIT;
    case 32515: // IDC_CROSS
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case 32642: // IDC_SIZENWSE
        return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case 32643: // IDC_SIZENESW
        return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case 32644: // IDC_SIZEWE
        return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case 32645: // IDC_SIZENS
        return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case 32640: // IDC_SIZE
    case 32646: // IDC_SIZEALL
        return SDL_SYSTEM_CURSOR_MOVE;
    case 32648: // IDC_NO
        return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    case 32649: // IDC_HAND
        return SDL_SYSTEM_CURSOR_POINTER;
    case 32650: // IDC_APPSTARTING
        return SDL_SYSTEM_CURSOR_PROGRESS;
    default: // IDC_ARROW, IDC_UPARROW, IDC_ICON, IDC_HELP
        return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

void sdl_guest_cursor_update(const HostCursorState &state) {
    if (g_applied && state.handle == g_applied_handle && state.chosen == g_applied_chosen)
        return;
    g_applied = true;
    g_applied_handle = state.handle;
    g_applied_chosen = state.chosen;
    // SetCursor(NULL) hides the pointer (main.cpp's capture code does that);
    // the shape underneath is the arrow.
    SDL_SetCursor(state.chosen && state.handle ? cursor_for(state.handle) : SDL_GetDefaultCursor());
}
