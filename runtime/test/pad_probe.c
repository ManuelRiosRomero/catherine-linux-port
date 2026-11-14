// Lists the joysticks/gamepads SDL3 sees, for 3 seconds (hot-plug + late enumeration). Run on the Deck.
#include <SDL3/SDL.h>
#include <stdio.h>
int main(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { printf("init: %s\n", SDL_GetError()); return 1; }
    for (int t = 0; t < 3; t++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_GAMEPAD_ADDED) printf("event: gamepad added %u\n", e.gdevice.which);
        int nj = 0, ng = 0;
        SDL_JoystickID *j = SDL_GetJoysticks(&nj), *g = SDL_GetGamepads(&ng);
        printf("t=%ds joysticks %d gamepads %d\n", t, nj, ng);
        for (int i = 0; i < nj; i++) printf("  joystick %u: %s (gamepad: %d)\n", j[i], SDL_GetJoystickNameForID(j[i]), SDL_IsGamepad(j[i]));
        SDL_free(j); SDL_free(g);
        SDL_Delay(1000);
    }
    return 0;
}
