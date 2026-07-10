#ifndef ASSET_MANAGER_H
#define ASSET_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>

struct AAssetManager {
    char base_path[256];
};

struct AAsset {
    FILE* fp;
    char name[256];
};

typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;

#ifdef __cplusplus
extern "C" {
#endif

// Simplified AAssetManager APIs
AAssetManager* AAssetManager_fromJava(void* env, void* assetManager);
AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);
int AAsset_read(AAsset* asset, void* buf, size_t count);
void AAsset_close(AAsset* asset);
off_t AAsset_getLength(AAsset* asset);
int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength);

// Internal setup
void asset_manager_init(const char* base_path);
const char* get_assets_base_path(void);

#ifdef __cplusplus
}
#endif

#endif
