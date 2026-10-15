// guest_cursor.h - the guest's cursor on the platform's pointer: the stock
// Windows cursors as SDL's system cursors, a program's own (CreateCursor,
// CreateIconIndirect) as SDL colour cursors with their hot spots, each made
// once per handle. Hiding stays with sdl/main.cpp's pointer capture.
#pragma once
#include "../../runtime/win32.h"

// Sets the SDL cursor to what `state` names (the platform's arrow when the
// guest has chosen none or the handle has no picture). Main thread; cheap
// when nothing changed.
void sdl_guest_cursor_update(const HostCursorState &state);
// The SDL system cursor (an SDL_SystemCursor value) a stock IDC_* number
// is shown as; the default arrow for one SDL has no likeness of.
int sdl_system_cursor_for_idc(uint32_t idc);
