// Profile

CppString identifier; // *$(CppString, profile, 0x0C, 0x18)

CppString name; // *$(CppString, profile, 0x10, 0x20)

DateTime lastPlayed; // *$(DateTime, profile, 0x18, 0x28)

int currentLevel; // *$(int, profile, 0x24, 0x38)

float percentCompleted; // *$(float, profile, 0x28, 0x40)

uint64_t playTime; // *$(uint64_t, profile, 0x30, 0x48)

void *gameState; // *$(void*, profile, 0x54, 0x78)

void *counterRoot; // *$(void*, profile, 0x68, 0x98)