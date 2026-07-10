#include "sre_config.h"
#include "toml.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern const char* sre_vfs_resolve_path(const char* path, char* out_buf);

int sre_config_load_toml(const char* toml_file, sre_config_t* out) {
    char real_path[512];
    sre_vfs_resolve_path(toml_file, real_path);
    
    FILE* fp = fopen(real_path, "r");
    if (!fp) return -1;
    
    char buf[8192];
    size_t bytes_read = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    
    buf[bytes_read] = '\0';
    
    char errbuf[200];
    toml_table_t* tab = toml_parse(buf, errbuf, sizeof(errbuf));
    if (!tab) return -2;
    
    memset(out, 0, sizeof(*out));
    out->coin_limit = 999;
    out->engine_speed = 1.0;
    
    /* mini.toml uses [mod_overlay] for mod identity (not [mod]) */
    toml_table_t* mod = toml_table_table(tab, "mod_overlay");
    if (mod) {
        toml_value_t name = toml_table_string(mod, "name");
        if (name.ok) {
            strncpy(out->mod_name, name.u.s, sizeof(out->mod_name) - 1);
            free(name.u.s);
        }
        toml_value_t version = toml_table_string(mod, "version");
        if (version.ok) {
            strncpy(out->mod_version, version.u.s, sizeof(out->mod_version) - 1);
            free(version.u.s);
        }
        /* 'authors' is a TOML array in mini.toml, e.g. authors = ["Name"] */
        toml_array_t* authors_arr = toml_table_array(mod, "authors");
        if (authors_arr) {
            /* Concatenate all author names separated by ", " */
            int n = toml_array_len(authors_arr);
            size_t written = 0;
            for (int i = 0; i < n && written < sizeof(out->mod_authors) - 1; i++) {
                toml_value_t a = toml_array_string(authors_arr, i);
                if (a.ok) {
                    if (i > 0 && written + 2 < sizeof(out->mod_authors) - 1) {
                        out->mod_authors[written++] = ',';
                        out->mod_authors[written++] = ' ';
                    }
                    size_t len = strlen(a.u.s);
                    if (written + len >= sizeof(out->mod_authors))
                        len = sizeof(out->mod_authors) - written - 1;
                    memcpy(out->mod_authors + written, a.u.s, len);
                    written += len;
                    out->mod_authors[written] = '\0';
                    free(a.u.s);
                }
            }
        } else {
            /* Fallback: try as plain string (non-standard mods) */
            toml_value_t authors = toml_table_string(mod, "authors");
            if (authors.ok) {
                strncpy(out->mod_authors, authors.u.s, sizeof(out->mod_authors) - 1);
                free(authors.u.s);
            }
        }
    }

    /* mini.toml uses [options] for gameplay options (not [config]) */
    toml_table_t* config = toml_table_table(tab, "options");
    if (config) {
        toml_value_t coin_limit = toml_table_int(config, "coin_limit");
        if (coin_limit.ok) {
            out->coin_limit = (int)coin_limit.u.i;
        }
        toml_value_t engine_speed = toml_table_double(config, "engine_speed");
        if (engine_speed.ok) {
            out->engine_speed = (float)engine_speed.u.d;
        }
    }

    
    toml_free(tab);
    return 0;
}
