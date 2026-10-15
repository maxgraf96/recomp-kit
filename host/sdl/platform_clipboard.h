// platform_clipboard.h - the platform's clipboard as the runtime's text
// bridge (runtime/win32.h HostClipboard), through SDL. SDL's clipboard calls
// belong to the main thread: from another one a paste finds no text, and a
// copy waits for the next sdl_clipboard_pump.
#pragma once

// Gives the runtime the bridge. Call after SDL_Init.
void sdl_clipboard_install();
// Publishes a copy a guest thread other than the main one made. Main thread.
void sdl_clipboard_pump();
