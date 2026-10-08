// Test-only SDL injection starts after the recorder writes its header.
#include <SDL.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>

extern "C" int SDL_PollEvent(SDL_Event* event) {
    using Poll = int (*)(SDL_Event*);
    static Poll real_poll = reinterpret_cast<Poll>(dlsym(RTLD_NEXT,"SDL_PollEvent"));
    static Uint32 origin = 0;
    static unsigned next = 0;
    if (!event) return real_poll(event);
    const char* path = std::getenv("NATIVE_REPLAY_PROBE_ARCHIVE");
    struct stat info = {};
    if (!origin && path && stat(path,&info) == 0 && info.st_size > 8) origin = SDL_GetTicks();
    static const Uint32 delays[] = {100,150,200,250,300,350,400,500,650,800,1000};
    if (origin && next < sizeof(delays)/sizeof(delays[0]) && SDL_GetTicks()-origin >= delays[next]) {
        std::memset(event,0,sizeof(*event));
        unsigned step = next++;
        if (step == 0 || step == 1 || step == 6 || step == 7) {
            event->type = step == 0 || step == 6 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            event->button.button = SDL_BUTTON_LEFT; event->button.state = event->type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            event->button.x = 150; event->button.y = 150;
        } else if (step == 2 || step == 3 || step == 9) {
            event->type = step == 3 ? SDL_KEYUP : SDL_KEYDOWN;
            event->key.state = event->type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
            event->key.keysym.scancode = step == 9 ? SDL_SCANCODE_B : SDL_SCANCODE_A;
            event->key.keysym.sym = step == 9 ? SDLK_b : SDLK_a;
        } else if (step == 4 || step == 5) {
            event->type = SDL_MOUSEMOTION;
            event->motion.x = 150 + int(step); event->motion.y = 150;
            event->motion.xrel = 4; event->motion.yrel = -2;
        } else if (step == 8) {
            event->type = SDL_MOUSEWHEEL; event->wheel.y = 1;
        } else {
            if (std::getenv("NATIVE_REPLAY_PROBE_ABORT")) std::_Exit(73);
            event->type = SDL_QUIT; // Hold B at close to test the synthesized release.
        }
        event->common.timestamp = SDL_GetTicks();
        return 1;
    }
    return real_poll(event);
}
