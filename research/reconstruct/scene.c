
// Scene (_G.scene)

void *camera; // *$(void*, scene, 0x240, 0x240) // main camera!

// Tint/color overlay // ...Scene.OverrideLights?
bool hasTint; // *$(bool, Scene, 0x280, 0x3f0)
float tintR; // *$(float, Scene, 0x284, 0x3f4)
float tintG; // *$(float, Scene, 0x288, 0x3f8)
float tintB; // *$(float, Scene, 0x28c, 0x3fc)
float tintA; // *$(float, Scene, 0x290, 0x400)

bool hitboxes; // *$(bool, Scene, 0x208, 0x358)