#ifndef SRE_FEATURES_H
#define SRE_FEATURES_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1. Weapon Trinket System */
typedef struct {
    char name[64];
    float r, g, b, a;
    float intensity;
    int active;
} SreWeaponTrinket;

void sre_trinket_init(void);
void sre_trinket_set_glow(const char* name, float r, float g, float b, float a, float intensity);
int  sre_trinket_get_glow(const char* name, float* r, float* g, float* b, float* a, float* intensity);

/* 2. Armor Damage & Defense System */
void sre_armor_set_defense_multiplier(float mult);
float sre_armor_get_defense_multiplier(void);
int  sre_armor_calculate_damage(int raw_damage, int armor_level);

/* 3. Coin Limit Extension */
void sre_set_max_coins(int max_coins);
int  sre_get_max_coins(void);

/* 4. Hero Model & Armor Swapper */
void sre_armor_set_model(const char* model_name, const char* texture_name);
const char* sre_armor_get_model_name(void);
const char* sre_armor_get_texture_name(void);

#ifdef __cplusplus
}
#endif

#endif /* SRE_FEATURES_H */
