#include "include/sre_features.h"
#include <string.h>
#include <stdio.h>

#define MAX_TRINKETS 32

static SreWeaponTrinket g_trinkets[MAX_TRINKETS];
static int g_trinket_count = 0;
static float g_armor_defense_mult = 1.0f;
static int g_max_coins = 999999;
static char g_armor_model_name[128] = "";
static char g_armor_texture_name[128] = "";

void sre_trinket_init(void) {
    memset(g_trinkets, 0, sizeof(g_trinkets));
    g_trinket_count = 0;
}

void sre_trinket_set_glow(const char* name, float r, float g, float b, float a, float intensity) {
    if (!name) return;
    for (int i = 0; i < g_trinket_count; i++) {
        if (strcmp(g_trinkets[i].name, name) == 0) {
            g_trinkets[i].r = r;
            g_trinkets[i].g = g;
            g_trinkets[i].b = b;
            g_trinkets[i].a = a;
            g_trinkets[i].intensity = intensity;
            g_trinkets[i].active = 1;
            return;
        }
    }
    if (g_trinket_count < MAX_TRINKETS) {
        strncpy(g_trinkets[g_trinket_count].name, name, 63);
        g_trinkets[g_trinket_count].r = r;
        g_trinkets[g_trinket_count].g = g;
        g_trinkets[g_trinket_count].b = b;
        g_trinkets[g_trinket_count].a = a;
        g_trinkets[g_trinket_count].intensity = intensity;
        g_trinkets[g_trinket_count].active = 1;
        g_trinket_count++;
    }
}

int sre_trinket_get_glow(const char* name, float* r, float* g, float* b, float* a, float* intensity) {
    if (!name) return 0;
    for (int i = 0; i < g_trinket_count; i++) {
        if (g_trinkets[i].active && strcmp(g_trinkets[i].name, name) == 0) {
            if (r) *r = g_trinkets[i].r;
            if (g) *g = g_trinkets[i].g;
            if (b) *b = g_trinkets[i].b;
            if (a) *a = g_trinkets[i].a;
            if (intensity) *intensity = g_trinkets[i].intensity;
            return 1;
        }
    }
    return 0;
}

void sre_armor_set_defense_multiplier(float mult) {
    if (mult > 0.0f) g_armor_defense_mult = mult;
}

float sre_armor_get_defense_multiplier(void) {
    return g_armor_defense_mult;
}

int sre_armor_calculate_damage(int raw_damage, int armor_level) {
    float reduction = 1.0f - (armor_level * 0.1f * g_armor_defense_mult);
    if (reduction < 0.1f) reduction = 0.1f;
    int final_dmg = (int)(raw_damage * reduction);
    return final_dmg > 1 ? final_dmg : 1;
}

void sre_set_max_coins(int max_coins) {
    if (max_coins > 0) g_max_coins = max_coins;
}

int sre_get_max_coins(void) {
    return g_max_coins;
}

void sre_armor_set_model(const char* model_name, const char* texture_name) {
    if (model_name) strncpy(g_armor_model_name, model_name, 127);
    if (texture_name) strncpy(g_armor_texture_name, texture_name, 127);
}

const char* sre_armor_get_model_name(void) {
    return g_armor_model_name;
}

const char* sre_armor_get_texture_name(void) {
    return g_armor_texture_name;
}
