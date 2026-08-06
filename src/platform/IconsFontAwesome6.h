// =============================================================================
// IconsFontAwesome.h — Font Awesome 6/7 icon definitions for ImGui
// Only includes icons actually used in the SwordigoDesktop launcher.
// Font file: src/assets/fontawesome/otfs/Font Awesome 7 Free-Solid-900.otf
//            (or src/assets/fonts/fa-solid-900.ttf for FA6)
// Codepoints are backward-compatible between FA6 and FA7.
// =============================================================================
#pragma once

// Font Awesome icon range (solid style) — covers both FA6 and FA7
#define ICON_FA_MIN      0xe000
#define ICON_FA_MAX      0xf8ff

// --- Navigation ---
#define ICON_FA_ARROW_LEFT       "\xef\x81\xa0"  // U+F060
#define ICON_FA_ARROW_RIGHT      "\xef\x81\xa1"  // U+F061
#define ICON_FA_CHEVRON_LEFT     "\xef\x81\x93"  // U+F053
#define ICON_FA_CHEVRON_RIGHT    "\xef\x81\x94"  // U+F054

// --- Actions ---
#define ICON_FA_PLAY             "\xef\x81\x8b"  // U+F04B
#define ICON_FA_XMARK            "\xef\x80\x8d"  // U+F00D
#define ICON_FA_PLUS             "\xef\x81\xa7"  // U+F067
#define ICON_FA_TRASH            "\xef\x87\xb8"  // U+F1F8
#define ICON_FA_PENCIL           "\xef\x8c\x83"  // U+F303
#define ICON_FA_CHECK            "\xef\x80\x8c"  // U+F00C
#define ICON_FA_MAGNIFYING_GLASS "\xef\x80\x82"  // U+F002

// --- Objects ---
#define ICON_FA_GEAR             "\xef\x80\x93"  // U+F013
#define ICON_FA_PAUSE            "\xef\x81\x8c"  // U+F04C
#define ICON_FA_STOP             "\xef\x81\x8d"  // U+F04D
#define ICON_FA_WARNING          "\xef\x81\xb1"  // U+F071
#define ICON_FA_MOUNTAIN_SUN     "\xef\x95\xbe"  // U+F6FE
#define ICON_FA_STAR             "\xef\x80\x85"  // U+F005
#define ICON_FA_FLOPPY_DISK      "\xef\x83\x87"  // U+F0C7
#define ICON_FA_FOLDER           "\xef\x81\xbb"  // U+F07B
#define ICON_FA_FOLDER_OPEN      "\xef\x81\xbc"  // U+F07C
#define ICON_FA_FILE             "\xef\x85\x9b"  // U+F15B
#define ICON_FA_IMAGE            "\xef\x80\xbe"  // U+F03E
#define ICON_FA_GAMEPAD          "\xef\x84\x9b"  // U+F11B
#define ICON_FA_SHIELD           "\xef\x84\xb2"  // U+F132
#define ICON_FA_WAND_SPARKLES    "\xef\x9c\xab"  // U+F72B
#define ICON_FA_BOLT             "\xef\x83\xa7"  // U+F0E7
#define ICON_FA_ROCKET           "\xef\x84\xb5"  // U+F135
#define ICON_FA_PUZZLE_PIECE     "\xef\x84\xae"  // U+F12E
#define ICON_FA_CUBE             "\xef\x86\xb2"  // U+F1B2
#define ICON_FA_HAND_POINTER     "\xef\x89\x9a"  // U+F25A
#define ICON_FA_DOWNLOAD         "\xef\x80\x99"  // U+F019

// --- Status / Info ---
#define ICON_FA_CIRCLE_INFO      "\xef\x81\x9a"  // U+F05A
#define ICON_FA_CIRCLE_CHECK     "\xef\x81\x98"  // U+F058
#define ICON_FA_TRIANGLE_EXCLAMATION "\xef\x81\xb1" // U+F071
#define ICON_FA_CIRCLE_XMARK     "\xef\x81\x97"  // U+F057
#define ICON_FA_CLOCK            "\xef\x80\x97"  // U+F017
#define ICON_FA_SPINNER          "\xef\x84\x90"  // U+F110

// --- Gaming / SRT specific ---
#define ICON_FA_SHIELD_HALVED    "\xef\x8f\xad"  // U+F3ED
#define ICON_FA_MICROCHIP        "\xef\x8b\x9b"  // U+F2DB
#define ICON_FA_GAUGE_HIGH       "\xef\x98\xa5"  // U+F625
#define ICON_FA_TERMINAL         "\xef\x84\xa0"  // U+F120
#define ICON_FA_CODE             "\xef\x84\xa1"  // U+F121
#define ICON_FA_PAINT_BRUSH      "\xef\x87\xbc"  // U+F1FC
#define ICON_FA_MUSIC            "\xef\x80\x81"  // U+F001
#define ICON_FA_VOLUME_HIGH      "\xef\x80\xa8"  // U+F028
#define ICON_FA_EYE              "\xef\x81\xae"  // U+F06E
#define ICON_FA_WRENCH           "\xef\x82\xad"  // U+F0AD
#define ICON_FA_SLIDERS          "\xef\x87\x9e"  // U+F1DE
#define ICON_FA_LAYER_GROUP      "\xef\x97\xbd"  // U+F5FD
#define ICON_FA_COPY             "\xef\x83\x85"  // U+F0C5

// --- Added for Remastered Launcher ---
#define ICON_FA_WINDOW_MINIMIZE  "\xef\x8b\x91"  // U+F2D1
#define ICON_FA_HOUSE            "\xef\x80\x95"  // U+F015
#define ICON_FA_USER             "\xef\x80\x87"  // U+F007
#define ICON_FA_LOCK             "\xef\x80\xa3"  // U+F023
#define ICON_FA_GLOBE            "\xef\x82\xac"  // U+F0AC
#define ICON_FA_PEN              "\xef\x8c\x84"  // U+F304
#define ICON_FA_CIRCLE_EXCLAMATION "\xef\x81\xaa" // U+F06A
#define ICON_FA_BOX_OPEN         "\xef\x92\x9e"  // U+F49E
#define ICON_FA_ARROWS_ROTATE    "\xef\x82\x9f"  // U+F021
#define ICON_FA_NEWSPAPER        "\xef\x87\xaa"  // U+F1EA
#define ICON_FA_CLOCK_ROTATE_LEFT "\xef\x87\xda" // U+F1F6
#define ICON_FA_LIST             "\xef\x80\xba"  // U+F03A
#define ICON_FA_TOOLBOX          "\xef\x92\x91"  // U+F491
#define ICON_FA_PLUG             "\xef\x8e\xac"  // U+F1E6
#define ICON_FA_SHIRT            "\xef\x95\x93"  // U+F553
#define ICON_FA_CLOUD            "\xef\x83\x82"  // U+F0C2
#define ICON_FA_DISPLAY          "\xef\x97\xbc"  // U+F5FC
#define ICON_FA_NETWORK_WIRED    "\xef\x9b\xbf"  // U+F6FF
#define ICON_FA_PAINTBRUSH       "\xef\x87\xbc"  // U+F1FC
#define ICON_FA_WAVEFORM         "\xef\x8f\xb6"  // U+F3F6
#define ICON_FA_PERSON_RUNNING   "\xef\x9c\x8c"  // U+F70C
#define ICON_FA_WAND_MAGIC_SPARKLES "\xef\x9c\xab" // U+F72B
#define ICON_FA_MAP              "\xef\x89\xb9"  // U+F279
#define ICON_FA_LOCATION_DOT     "\xef\x8f\x85"  // U+F3C5
#define ICON_FA_MAP_LOCATION_DOT "\xef\x96\x9f"  // U+F59F

