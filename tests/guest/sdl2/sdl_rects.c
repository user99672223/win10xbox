/* sdl_rects: a real SDL2 program (SDL2 2.30.9 built from the upstream release by tools/ci/build_sdl2.ps1) run through
 * the layer. Borderless 320x240 window at (100,100), software renderer: clear to blue, a red rect at (20,20,60,60),
 * a green rect at (120,40,50,50); polls events for 30 frames, reads its own pixels with SDL_RenderReadPixels, exits 42.
 * Integration test: integration.real_sdl2 (screenshot pixels at screen = client + (100,100)). */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>

static void draw(SDL_Renderer *r) {
    SDL_Rect a = {20, 20, 60, 60}, b = {120, 40, 50, 50};
    SDL_SetRenderDrawColor(r, 0, 80, 220, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
    SDL_RenderFillRect(r, &a);
    SDL_SetRenderDrawColor(r, 0, 255, 0, 255);
    SDL_RenderFillRect(r, &b);
}

int main(int argc, char **argv) {
    SDL_Window *w;
    SDL_Renderer *r;
    SDL_RendererInfo info;
    int events = 0, frame;
    (void)argc; (void)argv;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("sdl: init failed: %s\n", SDL_GetError()); return 2; }
    printf("sdl: init video=%s\n", SDL_GetCurrentVideoDriver());
    fflush(stdout);
    w = SDL_CreateWindow("win10xbox sdl", 100, 100, 320, 240, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!w) { printf("sdl: window failed: %s\n", SDL_GetError()); return 3; }
    r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { printf("sdl: renderer failed: %s\n", SDL_GetError()); return 4; }
    SDL_GetRendererInfo(r, &info);
    printf("sdl: renderer=%s\n", info.name);
    fflush(stdout);
    for (frame = 0; frame < 30; ++frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) ++events;
        draw(r);
        if (frame == 29) {
            Uint32 px[3] = {0, 0, 0};
            SDL_Rect p0 = {5, 5, 1, 1}, p1 = {30, 30, 1, 1}, p2 = {130, 50, 1, 1};
            SDL_RenderReadPixels(r, &p0, SDL_PIXELFORMAT_ARGB8888, &px[0], 4);
            SDL_RenderReadPixels(r, &p1, SDL_PIXELFORMAT_ARGB8888, &px[1], 4);
            SDL_RenderReadPixels(r, &p2, SDL_PIXELFORMAT_ARGB8888, &px[2], 4);
            printf("sdl: pixels %08x %08x %08x\n", (unsigned)px[0], (unsigned)px[1], (unsigned)px[2]);
        }
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    printf("sdl: frames=30 events=%d\n", events);
    printf("sdl: done\n");
    fflush(stdout);
    /* Exit without destroying the window so the host's screenshot holds the last frame. */
    return 42;
}
