#ifndef ASSET_MANAGER_ARM32_H
#define ASSET_MANAGER_ARM32_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>

struct AAssetManager_arm32 {
    char base_path[256];
};

struct AAsset_arm32 {
    FILE* fp;
    char name[256];
};

typedef struct AAssetManager_arm32 AAssetManager_arm32;
typedef struct AAsset_arm32 AAsset_arm32;

#ifdef __cplusplus
extern "C" {
#endif

// Simplified ARM32 AAssetManager APIs
AAssetManager_arm32* AAssetManager_fromJava_arm32(void* env, void* assetManager);
AAsset_arm32* AAssetManager_open_arm32(AAssetManager_arm32* mgr, const char* filename, int mode);
int AAsset_read_arm32(AAsset_arm32* asset, void* buf, size_t count);
void AAsset_close_arm32(AAsset_arm32* asset);
off_t AAsset_getLength_arm32(AAsset_arm32* asset);
int AAsset_openFileDescriptor_arm32(AAsset_arm32* asset, off_t* outStart, off_t* outLength);

// Internal setup
void asset_manager_init_arm32(const char* base_path);

#ifdef __cplusplus
}
#endif

#endif
