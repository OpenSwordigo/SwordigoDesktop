Vanilla (arm64)
-> Mild fram drops all throught the game maybe(MY GUESS ONLY) due to the Caver::ProgramState::Update 's implementation of ours. (see second point) fucntion we hooked , every times works accurately but we should fixc lag with say host side update loop via sre host relations / performance oriented implementation of the same function
->  we need to fix the sre_ProgramState_Update (our sre naming) / _ZN5Caver12ProgramState6UpdateEf(maligned name) / Caver::ProgramState::Update(original name) the real function is present in "/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDecompiled/GhidraDecomp src/game_systems/ProgramState.c" line 321.

-> Memory leaks are high for the below session , Memory Leaks accumulated and Became 1.8GB when i close process.
========================================
[RESULT64] Completed 63335 frames (ARM64)
========================================
  Total draw calls:      5579755
  Total tex binds:       2662720
  Avg draw/frame:        88
========================================
[IO Async] Background thread stopped
[Main] Swordigo — session complete.
-> No FREEZES / CRAHSES IN VANILLA , ALL BEHAVIOR IVE CHECK SO FAR WORKED CORRECTLY.
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------
RLSwordigo v7 (arm64)
-> Takes more time to load the hiro scene (or the level selection / world selection screen) which appears after we press start in MainMenuScreen.
-> Portal / Chest System and so on, and other minute events/entity system related failiures.
-> Insane Memory Leaks , for the below seesion, it reached 3.2GB , near the 3.5GB heap size , it will crash when it reaches 3.5GB , if we cannot fix it, we shoudl atleast try to reduces memory leaks in Vanilla and RLSW, not even required to fully fix now itslef, but before next push.
========================================
[RESULT64] Completed 23495 frames (ARM64)
========================================
  Total draw calls:      2028280
  Total tex binds:       1019278
  Avg draw/frame:        86
========================================
[IO Async] Background thread stopped
[Main] Swordigo — session complete.
-> Character / NPC custom talking dialogues are not visible and not interabale (Custom NPC Interaction mechanism used by RLSW)
-> RLSW 6.6 didnt launch for some reaosn (maybe we updated some core API whcih it depends old one, or a problem in our side) , NOT URGENT BUT RLSW 7 IS STILL IN BETA , UNRELEASED, so MOST Players will have access to v6.6 , we can try, but not mandatory to keep it working)
-> The Custom GUI panel Swordfare GUI , misbehaves when complex GUI works are done , not complex when even if we do something , the system is a total failiure, we need to fix all bugs , sometime the overlay isnt closing , and the inventory system is not yet fixed, the RLSW isnt storing custom playerdata as it do on mobile (non vanilla player data) so inevntory system broke, also keybinds didnt work , RLSW has custom items which we can keep as keybinds , we can later assign 5 6 7 for them (3 keybinds is max in rlsw) and 1 2 3 4 are already used for vanilla powerups / items / magic whatever we say.
-> Some MORE ISSUES with Custom GUI , and stuff
-> Lags , same , MY GUESS IS THAT we need to fix the sre_ProgramState_Update (our sre naming) / _ZN5Caver12ProgramState6UpdateEf(maligned name) / Caver::ProgramState::Update(original name) the real function is present in "/run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDecompiled/GhidraDecomp src/game_systems/ProgramState.c" line 321.
-> Wierd behavior in some minute areas.
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------Combatch v3 (arm64)
-> Works perfectly (base combatch game)
-> GUI issues is same linked to RLSW , coz all of these mods uses Kiwi API , we need to stablise Kiwi/Mini API , then all of these GUI would work properly.
-------------------
REFERENCES YOU CAN TAKE INFO FROM->
1) SwKiwi Source code [use the new kiwi sc] - /home/quantumcreeper/SwordigoDesktop/reference/SwKiwi-main new version
2) SwordigoDecompiled/GhidraDecomp - /run/media/quantumcreeper/TVPG/Prenxy Packages/SwordigoDecompiled/GhidraDecomp src/
3) RLSWDECODED - decoded scl's using FIleRift - /home/quantumcreeper/SwordigoDesktop/reference/rlswscldecoded
---------------------
OBSERVATION

Vanilla exhibits mild frame drops.

HYPOTHESIS

I suspect our implementation of
sre_ProgramState_Update()
is responsible.

This is NOT confirmed.
Please profile before modifying.
======================
Goal:

Do NOT redesign systems.

Focus on:

1. Stability
2. Memory usage
3. Performance
4. Compatibility

Behavioural compatibility with Vanilla
takes precedence over adding features.
