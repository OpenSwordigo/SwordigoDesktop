// GameViewController: (64? bit only for now)
void *stackController;    // gvc + 0x40  // view stack controller
void *gameObject;         // gvc + 0x88  // main game object (has timer at +0x40)
GameSceneController *gsc; // gvc + 0xc8  // GameSceneController (200 = 0xc8 in 32bit)
GameState *gameState;     // gvc + 0xa8  // current game state
void *currentScene;       // gvc + 0xd8  // current scene shared_ptr
void *nextScene;          // gvc + 0xe0  // pending scene transition
bool hasNextScene;        // gvc + 0xe8  // scene transition pending
bool isTransitioning;     // gvc + 0xe9  // currently transitioning
float guideTimer;         // gvc + 0x130 // guide target update timer
float fadeTimer;          // gvc + 0x84  // fade/delay timer
int levelUpState;         // gvc + 0x80  // 0=none, 1=fall, 2=levelup
bool moviePlaying;        // gvc + 0x110 // cutscene playing flag
void *pendingScene;       // gvc + 0x100 // pending scene load pointer
void *pendingSceneRef;    // gvc + 0x108 // pending scene ref count