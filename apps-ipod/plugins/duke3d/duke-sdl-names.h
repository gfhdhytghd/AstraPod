/* Isolate the SDL 1.2 hooks from the host SDL2 symbols. GPL v2+. */
#ifndef ASTRAPOD_DUKE_SDL_NAMES_H
#define ASTRAPOD_DUKE_SDL_NAMES_H

#define SDL_ClearError duke_SDL_ClearError
#define SDL_CreateMutex duke_SDL_CreateMutex
#define SDL_Delay duke_SDL_Delay
#define SDL_DestroyMutex duke_SDL_DestroyMutex
#define SDL_FillRect duke_SDL_FillRect
#define SDL_Flip duke_SDL_Flip
#define SDL_GetError duke_SDL_GetError
#define SDL_GetMouseState duke_SDL_GetMouseState
#define SDL_GetTicks duke_SDL_GetTicks
#define SDL_GetVideoInfo duke_SDL_GetVideoInfo
#define SDL_Init duke_SDL_Init
#define SDL_Linked_Version duke_SDL_Linked_Version
#define SDL_ListModes duke_SDL_ListModes
#define SDL_LockSurface duke_SDL_LockSurface
#define SDL_PollEvent duke_SDL_PollEvent
#define SDL_Quit duke_SDL_Quit
#define SDL_QuitSubSystem duke_SDL_QuitSubSystem
#define SDL_SetColors duke_SDL_SetColors
#define SDL_SetModState duke_SDL_SetModState
#define SDL_SetVideoMode duke_SDL_SetVideoMode
#define SDL_ShowCursor duke_SDL_ShowCursor
#define SDL_UnlockSurface duke_SDL_UnlockSurface
#define SDL_UpdateRect duke_SDL_UpdateRect
#define SDL_VideoDriverName duke_SDL_VideoDriverName
#define SDL_WM_GrabInput duke_SDL_WM_GrabInput
#define SDL_WM_SetCaption duke_SDL_WM_SetCaption
#define SDL_WarpMouse duke_SDL_WarpMouse
#define SDL_mutexP duke_SDL_mutexP
#define SDL_mutexV duke_SDL_mutexV

#endif
