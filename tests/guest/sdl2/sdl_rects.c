/* sdl_rects: a real SDL2 program run through the layer, linked against the OFFICIAL SDL2 2.30.9 x64 release
 * (SDL2.dll + SDL2.lib from SDL2-devel-2.30.9-VC.zip, tools/ci/build_sdl_guest.ps1). Borderless 320x240 window at
 * (100,100), software drawing: SDL_GetWindowSurface + SDL_FillRect (blue clear, red rect (20,20,60,60), green rect
 * (120,40,50,50)) + SDL_UpdateWindowSurface; then, like tests/guest/window.c, it reads its own pixels back through
 * the window's HWND (SDL_GetWindowWMInfo, GetDC, GetPixel) and exits 42 only if they match. The window is never
 * destroyed so the host's screenshot holds the frame. Test: integration.real_sdl2. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>

#define BLUE  RGB(0, 80, 220)
#define RED   RGB(255, 0, 0)
#define GREEN RGB(0, 255, 0)

int main(int argc, char **argv) {
    SDL_Window *w;
    SDL_Surface *s;
    SDL_SysWMinfo wm;
    SDL_Rect a = {20, 20, 60, 60}, b = {120, 40, 50, 50};
    HDC dc;
    COLORREF p[3];
    int frame;
    (void)argc; (void)argv;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("sdl: init failed: %s\n", SDL_GetError()); return 2; }
    printf("sdl: init video=%s\n", SDL_GetCurrentVideoDriver());
    fflush(stdout);
    w = SDL_CreateWindow("win10xbox sdl", 100, 100, 320, 240, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!w) { printf("sdl: window failed: %s\n", SDL_GetError()); return 3; }
    s = SDL_GetWindowSurface(w);
    if (!s) { printf("sdl: surface failed: %s\n", SDL_GetError()); return 4; }
    printf("sdl: surface %dx%d\n", s->w, s->h);
    fflush(stdout);
    for (frame = 0; frame < 5; ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 80, 220));
        SDL_FillRect(s, &a, SDL_MapRGB(s->format, 255, 0, 0));
        SDL_FillRect(s, &b, SDL_MapRGB(s->format, 0, 255, 0));
        if (SDL_UpdateWindowSurface(w) != 0) { printf("sdl: update failed: %s\n", SDL_GetError()); return 5; }
        SDL_Delay(16);
    }
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(w, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS) { printf("sdl: wminfo failed: %s\n", SDL_GetError()); return 6; }
    dc = GetDC(wm.info.win.window);
    p[0] = GetPixel(dc, 5, 5);
    p[1] = GetPixel(dc, 30, 30);
    p[2] = GetPixel(dc, 130, 50);
    ReleaseDC(wm.info.win.window, dc);
    printf("sdl: pixels %08lx %08lx %08lx\n", (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2]);
    printf("sdl: done\n");
    fflush(stdout);
    return (p[0] == BLUE && p[1] == RED && p[2] == GREEN) ? 42 : 7;
}
