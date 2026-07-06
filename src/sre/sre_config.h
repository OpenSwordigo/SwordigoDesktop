#ifndef SRE_CONFIG_H
#define SRE_CONFIG_H

typedef struct {
    char mod_name[256];
    char mod_version[64];
    char mod_authors[512];
    int coin_limit;
    float engine_speed;
} sre_config_t;

int sre_config_load_toml(const char* toml_file, sre_config_t* out);

#endif /* SRE_CONFIG_H */
