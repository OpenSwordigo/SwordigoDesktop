// GameSceneController:

void *HeroState = *$(void*, gsc, 0x04, 0x08);

void *Scene = *$(void*, gsc, 0x10, 0x20);

CameraController *Camera = (CameraController *)((uintptr_t)gsc + 0x1C);

void *HeroObject = *$(void*, gsc, 0xA4, 0xD8);

void *HeroIndicator = *$(void*, gsc, 0xA8, 0xE0);

void *CharController = *$(void*, gsc, 0xAC, 0xE8);

void *TargetObject = *$(void*, gsc, 0xB0, 0xF0);

void *TargetObject2 = *$(void*, gsc, 0xB4, 0xF8);

void *CastObject = *$(void*, gsc, 0xEC, 0x158);

void *CastTarget = *$(void*, gsc, 0xF0, 0x160);

void *MonsterTarget = *$(void*, gsc, 0xF8, 0x170);