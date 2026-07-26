DEEP INVESTIGATION: Swordigo Save System — Complete Report
EXECUTIVE SUMMARY (bro dont use cli tools, tell me)
Swordigo uses TWO separate persistence mechanisms:

Google Play Games Snapshots — for game world state (the main save data blob)
Android SharedPreferences — for settings/flags like age, privacy consent, foreground time
The native C++ code does NOT directly save — it delegates ALL persistence to Java via JNI calls. The Java side then uses Google Play Games SnapshotsClient API.

1. THE SNAPSHOT SYSTEM (Game Save Data)
How it works — Full Flow:
A) Native → Java Method Signatures (from Native.java lines 346-383):

java

// Native.java line 75 — THE CALLBACK (native → Java → native)
public static native void snapshotLoaded(String str, byte[] bArr);
// Native.java line 346 — Load request
public static void loadSnapshot(final String str, final double d)  // str=name, d=delay_seconds
// Native.java line 365 — Save request  
public static void saveSnapshot(final String str, final byte[] bArr, final String str2, final long j, final long j2)
// str=snapshot_name, bArr=data_bytes, str2=description, j=time_played_millis, j2=progress_value
// Native.java line 375 — Delete
public static void deleteSnapshot(final String str)  // str=name
B) What the native .so exports (JNI symbols):

Java_com_touchfoo_swordigo_Native_snapshotLoaded — receives loaded data back from Java
The native code calls loadSnapshot, saveSnapshot, deleteSnapshot as Java static methods
C) snapshotLoaded callback signature:

void snapshotLoaded(JNIEnv* env, jobject obj, jstring name, jbyteArray data)
4 args: env, this, snapshot_name (can be null), byte_array_data (null = no save)
When data is null → new game (no existing save)
When data is non-null → the game deserializes this byte[] to restore world state
D) Data Format:

The save data is an opaque byte array (byte[])
The native engine serializes/deserializes it internally
Via Google Play Games API, it's stored as Snapshot.getSnapshotContents().readFully() → byte[]
Metadata includes: description string, time played (millis), progress value (long)
How GameServices.java Handles It (lines 123-244):

loadSnapshot(name) → openSnapshot(name, false) → readFully() → calls Native.snapshotLoaded(name, bytes)
loadAllSnapshots() → loadAllSnapshotsMetadata() → iterates all → loads each → calls Native.snapshotLoaded per snapshot
saveSnapshot(name, data, desc, time, progress) → openSnapshot(name, true) → writeBytes(data) → commitAndClose()
deleteSnapshot(name) → loadAllSnapshotsMetadata() → find matching → snapshotsClient.delete()
2. SharedPreferences (Settings/Flags — NOT game world state)
From Native.java lines 141-159, the native code calls these Java static methods:

java

// Preference name: "SwordigoPreferences"
public static boolean getBooleanFromSP(String str)  // key → boolean
public static void saveBooleanInSP(String str, boolean z)  // key, value
public static int getIntFromSP(String str)  // key → int  
public static void saveIntInSP(String str, int i)  // key, value
Known keys stored in SharedPreferences:

Key	Type	Purpose
knownAge	int	User's entered age (0=unknown)
privacyConsent	boolean	Privacy consent obtained
explicitConsent	boolean	Explicit consent obtained
totalForegroundTime	long	App foreground time tracking
foregroundTimeForReviewFlow	long	For review prompt timing
delayToReviewFlow	long	Review flow delay
These are NOT game save data — they're app-level settings. The native code calls these as JNI static methods on the Native class.

⚠ CRITICAL: These SharedPreferences methods are called FROM the native .so code via JNI GetStaticMethodID → CallStaticObjectMethodV. They're NOT among the methods currently handled in our jni_bridge.cpp!

3. OUR CURRENT DESKTOP PORT IMPLEMENTATION
jni_bridge.cpp — Current save handling (lines 1125-1178):
cpp

// GetMethodID assigns IDs:
// loadSnapshot  → 0x13250001
// saveSnapshot  → 0x13250002  
// deleteSnapshot → 0x13250003
// Global state:
bool g_snapshot_load_pending = false;        // line 823
std::vector<uint8_t> g_snapshot_data;        // line 824
bool g_snapshot_has_data = false;            // line 825
// In bridge_CallStaticVoidMethodV:
case 0x13250001: // loadSnapshot
    // Reads g_save_dir + "/snapshot.bin" from disk
    // Sets g_snapshot_load_pending = true
    
case 0x13250002: // saveSnapshot  
    // Reads byte[] from va_list args (name_ref, data_ref)
    // Array layout: [4-byte length][N bytes data]
    // Writes to g_save_dir + "/snapshot.bin"
case 0x13250003: // deleteSnapshot
    // Removes g_save_dir + "/snapshot.bin"
main.cpp — Async callback (lines 403-422):
cpp

// After each updateApplication() call:
if (g_snapshot_load_pending && snapshotLoaded) {
    g_snapshot_load_pending = false;
    if (g_snapshot_has_data && !g_snapshot_data.empty()) {
        // Write array to guest memory at 0x40000000: [4-byte len][data]
        g_emulator->call(snapshotLoaded, {env_ptr, 0, 0, save_buf_addr});
        // Args: env, this=0, name=null(0), data=array_addr
    } else {
        g_emulator->call(snapshotLoaded, {env_ptr, 0, 0, 0});
        // Args: env, this=0, name=null, data=null → new game
    }
}
g_save_dir is defined in jni_bridge.cpp line 26: std::string g_save_dir = "./save"; It's declared extern in main.cpp line 37.

4. VITA PORT SAVE HANDLING
The Vita port does NOT implement snapshot JNI methods at all! It has:

setFilesDir called with "ux0:data/swordigo" (line 1780)
fopen_hook at line 539 redirects file paths to data_path/assets/
The Vita port relies on the native engine's internal file I/O for saves
The engine uses fopen/fwrite/fread directly to getFilesDir() path
The fopen_hook redirects relative paths to ux0:data/swordigo/assets/
KEY INSIGHT: The Vita port shows the native engine ALSO does direct file I/O saving (via fopen) in addition to the snapshot API. The fopen_hook redirects paths to the Vita data directory. The snapshot system is for Google Play Games cloud sync — the engine likely has a local fallback that uses direct file I/O.

5. MISSING PIECES IN OUR DESKTOP PORT
A) SharedPreferences methods are NOT implemented: The native code calls getBooleanFromSP, saveBooleanInSP, getIntFromSP, saveIntInSP via GetStaticMethodID → CallStaticBooleanMethodV/CallStaticVoidMethodV. These are Java-side static methods that the native code invokes. Our bridge doesn't handle them because they're looked up via GetStaticMethodID with method names like "getBooleanFromSP", "getIntFromSP", etc. — but our bridge_GetMethodID / bridge_GetStaticMethodID only maps a limited set of names.

B) Potential issue with save data byte array layout: Our saveSnapshot handler at line 1158 reads *(uint32_t*)(memory + data_ref) as array length. This assumes the JNI array layout [length][data]. But the game might pass a jbyteArray handle instead. Need to verify.

C) The snapshotLoaded callback passes data at fixed address 0x40000000: This is a static address. If called multiple times, data gets overwritten in place. Probably fine since loads should be sequential.

6. KEY FILE LOCATIONS
File	Path
Native.java (decompiled)	.../decompClasses/classexdex2/app/src/main/java/com/touchfoo/swordigo/Native.java
GameServices.java	Same directory — GameServices.java
PersistentState.java	Same directory — PersistentState.java
Vita port main.c	.../docs/research/vita/vistaPort/swordigo-vita-master/loader/main.c
Our jni_bridge.cpp	src/jni/jni_bridge.cpp (save code at lines 823-826, 1125-1178)
Our main.cpp	src/main.cpp (callback at lines 286, 403-422)
Note: The symbols_sorted.txt file appears to be empty or not contain the expected symbol data — all grep searches against it returned no results.

The comprehensive report has been sent to the main agent. It covers:

Snapshot system - Complete flow from native → Java → Google Play Games and back via snapshotLoaded callback
SharedPreferences - All the settings/flags the game stores (age, consent, timing) with keys and types
snapshotLoaded signature - (JNIEnv*, jobject, jstring name, jbyteArray data) - 4 args, null data = new game
Current desktop port implementation - How jni_bridge.cpp handles save/load with snapshot.bin
Vita port approach - No snapshot JNI at all; reli No snapshot JNI at all; relies on native engine's direct fopen file I/O
Missing pieces - SharedPreferences Java methods not bridged, potential byte array layout issues