#include "asset_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define fileno _fileno
#else
#include <unistd.h>
#endif

static struct AAssetManager g_mgr;

// Exposed to jni_bridge.cpp so we can tag which GL texture ID corresponds to which asset
char g_last_opened_asset[256] = {0};

void asset_manager_init(const char* base_path) {
    strncpy(g_mgr.base_path, base_path, sizeof(g_mgr.base_path));
}

AAssetManager* AAssetManager_fromJava(void* env, void* assetManager) {
    return &g_mgr;
}

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
    char full_path[512];
    
    // Track last opened filename for texture identification in jni_bridge
    strncpy(g_last_opened_asset, filename, sizeof(g_last_opened_asset) - 1);
    g_last_opened_asset[sizeof(g_last_opened_asset) - 1] = '\0';
    
    printf("OPEN: %s\n", filename);
    
    if (strstr(filename, ".scene") != NULL) {
        extern void reset_void_fill_color(void);
        reset_void_fill_color();
    }
    
    if (strstr(filename, "swordigo_title_2x.pvr") != NULL) {
        printf("\n***************************\n");
        printf("* RENDERING PHASE REACHED *\n");
        printf("***************************\n\n");
    }

    // === MOD OVERLAY: Check mod directories first ===
    // Scan ~/.local/share/swordigo-desktop/mods/<modname>/assets/<filename>
    // Skip disabled mods (dot-prefixed folders).
    // This allows mods to replace ANY game asset: scenes, textures, models, etc.
    {
        extern char* get_user_data_dir_c(void);
        char* data_dir = get_user_data_dir_c();
        if (data_dir) {
            char mods_path[512];
            snprintf(mods_path, sizeof(mods_path), "%s/mods", data_dir);
            
            DIR* mods_dir = opendir(mods_path);
            if (mods_dir) {
                struct dirent* de;
                while ((de = readdir(mods_dir)) != NULL) {
                    // Skip ., .., and disabled mods (dot-prefixed)
                    if (de->d_name[0] == '.') continue;
                    
                    char mod_asset_path[512];
                    snprintf(mod_asset_path, sizeof(mod_asset_path),
                             "%s/%s/assets/%s", mods_path, de->d_name, filename);
                    
                    FILE* mod_fp = fopen(mod_asset_path, "rb");
                    if (mod_fp) {
                        printf("MOD OVERRIDE: %s -> %s\n", filename, mod_asset_path);
                        AAsset* asset = (AAsset*)malloc(sizeof(AAsset));
                        asset->fp = mod_fp;
                        strncpy(asset->name, filename, sizeof(asset->name));
                        closedir(mods_dir);
                        return asset;
                    }
                }
                closedir(mods_dir);
            }
        }
    }
    
    // If filename starts with ./, don't prepend base_path
    if (filename[0] == '.' && filename[1] == '/') {
        strncpy(full_path, filename, sizeof(full_path));
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", mgr->base_path, filename);
    }

    FILE* fp = fopen(full_path, "rb");
    if (!fp) {
        printf("OPEN FAILED: %s\n", full_path);
        return NULL;
    }
    
    printf("OPEN SUCCESS: %s\n", full_path);
    
    AAsset* asset = (AAsset*)malloc(sizeof(AAsset));
    asset->fp = fp;
    strncpy(asset->name, filename, sizeof(asset->name));
    return asset;
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
    if (!asset || !asset->fp) return -1;
    int read = fread(buf, 1, count, asset->fp);
    printf("READ: %s (%d bytes)\n", asset->name, read);
    return read;
}

void AAsset_close(AAsset* asset) {
    if (asset) {
        printf("CLOSE: %s\n", asset->name);
        if (asset->fp) fclose(asset->fp);
        free(asset);
    }
}

off_t AAsset_getLength(AAsset* asset) {
    if (!asset || !asset->fp) return 0;
    long current = ftell(asset->fp);
    fseek(asset->fp, 0, SEEK_END);
    off_t len = ftell(asset->fp);
    fseek(asset->fp, current, SEEK_SET);
    printf("GETLEN: %s -> %ld\n", asset->name, (long)len);
    return len;
}


int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (!asset || !asset->fp) return -1;
    *outStart = 0;
    *outLength = AAsset_getLength(asset);
    return dup(fileno(asset->fp));
}
