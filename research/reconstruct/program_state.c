
typedef struct ProgramState {
	lua_State *L;
	/* There's a lot more to this, but L is first. */
} ProgramState;

void *program_state_from_L(lua_State *L); // #include "caver/program_state.h"

// ProgramState

void *obj = *$(void*, ProgramState, 0x10, 0x20); // somehow... always 0x0? NULL.

int isWaiting = *$(int, ProgramState, 0x24, 0x48);
float waitTime = *$(float, ProgramState, 0x28, 0x4c);
float speedMultiplier = *$(float, ProgramState, 0x30, 0x54);