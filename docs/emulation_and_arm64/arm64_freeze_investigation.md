# ARM64 Freeze Investigation - ROOT CAUSE FOUND

## Root Cause: LDAXR/STLXR Exclusive Monitor Not Handled

Unicorn Engine does NOT properly handle ARM64 exclusive monitor instructions:
- **LDAXR** (Load-Acquire Exclusive Register)
- **STLXR** (Store-Release Exclusive Register)

In single-threaded Unicorn, `STLXR` always fails (status=1), making **every atomic CAS loop infinite**.

### Impact
The ARM64 binary uses LDAXR/STLXR everywhere:
- `boost::shared_ptr` refcount decrement
- `std::string` destructor (reference counted)
- All mutex/spinlock operations
- Every `ExclusiveMonitorPass` / `ExclusiveMonitorsStatus` call in Ghidra

This is why:
- Entity processing spins at 0x581164 (shared_ptr release)
- MusicPlayer hangs (shared_ptr release)
- Scene loading never completes

### Fix
Patch all STLXR instructions in the binary to STR + MOV Ws, #0 (always succeed).
Or add a Unicorn code hook to intercept STLXR and force Ws=0.

The cleanest approach: scan the loaded binary for STLXR patterns and replace with non-exclusive stores.
