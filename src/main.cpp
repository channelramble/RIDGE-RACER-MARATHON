// Entry point. SDL's main-callback model works the same on desktop, Android and iOS.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/App.h"

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    *appstate = &rrm::app();
    return rrm::app().init(argc, argv) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void*) {
    return rrm::app().iterate() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppEvent(void*, SDL_Event* event) {
    return rrm::app().handleEvent(*event) ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

void SDL_AppQuit(void*, SDL_AppResult) {
    rrm::app().shutdown();
}
