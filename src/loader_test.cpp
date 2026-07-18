#include "loader/elf_loader.h"
#include <iostream>
#include <vector>
#include <string>

// Globals needed by jni_bridge (normally in main.cpp)
std::string g_assets_dir = "assets";
std::string g_instance_assets_dir = "assets";

int main() {
    uint32_t guest_size = 0x10000000; // 256MB
    uint8_t* guest_base = new uint8_t[guest_size];
    
    ElfLoader loader(guest_base, guest_size);
    so_module mod;
    
    // Test loading libswordigo.so (1.4.6)
    // Use a safe load address within the 256MB buffer
    int res = loader.load(&mod, "reference/lib/swordigo 1.4.6/armeabi-v7a/libswordigo.so", 0x1000000); 
    if (res != 0) {
        std::cerr << "Failed to load SO: " << res << std::endl;
        return 1;
    }
    
    std::cout << "Successfully loaded SO at " << (void*)mod.base_addr << std::endl;
    std::cout << "Text base: " << (void*)mod.text_base << ", size: " << mod.text_size << std::endl;
    
    res = loader.relocate(&mod);
    if (res != 0) {
        std::cerr << "Failed to relocate SO: " << res << std::endl;
        return 1;
    }
    std::cout << "Relocated SO" << std::endl;
    
    uint32_t setupApp = loader.get_symbol_vaddr(&mod, "Java_com_touchfoo_swordigo_Native_setupApplication");
    std::cout << "setupApplication at: " << (void*)setupApp << std::endl;
    
    if (setupApp == 0) {
        std::cerr << "Could not find setupApplication!" << std::endl;
        return 1;
    }

    return 0;
}
