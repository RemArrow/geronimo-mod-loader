// Minimal x64 inline hooking.
//
// Target prologue is overwritten with a 5-byte `jmp rel32` to a relay that lives within
// +-2GB of the target; the relay does an absolute jmp to the detour. The stolen
// instructions are copied into a trampoline (relocating RIP-relative operands and
// rel8/rel32 branches) followed by an absolute jmp back into the target.
//
// The length decoder deliberately refuses anything it does not fully understand;
// a refused hook is logged, never guessed.
#include "internal.h"
#include <map>
#include <mutex>

namespace gml {

// ------------------------------------------------------------------ length decoder

struct Insn {
    size_t len = 0;
    int    ripDispOff = -1;  // offset of a RIP-relative disp32, or -1
    int    relOff = -1;      // offset of a branch displacement, or -1
    int    relSize = 0;      // 1 or 4
    uint8_t op = 0;          // primary opcode (after 0F if two-byte)
    bool   twoByte = false;
};

static bool Has(const uint8_t* set, size_t n, uint8_t b) {
    for (size_t i = 0; i < n; i++) if (set[i] == b) return true;
    return false;
}

// Decode ModRM (+SIB +disp) at p; returns bytes consumed, sets ripDisp relative to p.
static size_t ModRM(const uint8_t* p, bool addr32, int* ripDisp) {
    (void)addr32;
    uint8_t m = p[0];
    uint8_t mod = m >> 6, rm = m & 7;
    size_t n = 1;
    if (mod == 3) return n;
    if (rm == 4) {                       // SIB
        uint8_t sib = p[1];
        n++;
        if ((sib & 7) == 5 && mod == 0) n += 4;
    } else if (rm == 5 && mod == 0) {    // RIP-relative disp32
        *ripDisp = (int)n;
        return n + 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

static bool Decode(const uint8_t* code, Insn& out) {
    const uint8_t* p = code;
    bool opsz = false, adsz = false, rexW = false;
    static const uint8_t prefixes[] = {0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66, 0x67};
    while (Has(prefixes, sizeof prefixes, *p)) {
        if (*p == 0x66) opsz = true;
        if (*p == 0x67) adsz = true;
        p++;
        if (p - code > 14) return false;
    }
    bool rex = false;
    if ((*p & 0xF0) == 0x40) { rex = true; rexW = (*p & 8) != 0; p++; }

    int rip = -1;
    uint8_t op = *p++;
    out.op = op;
    if (rex && (op == 0xC4 || op == 0xC5 || op == 0x62)) return false;  // REX cannot precede VEX/EVEX
    if (op == 0x8F && ((*p >> 3) & 7) != 0) return false;                // 8F /1-7 is AMD XOP, not pop

    // VEX (C4/C5) and EVEX (62): always vector ops in 64-bit mode.
    if (op == 0xC5 || op == 0xC4 || op == 0x62) {
        int map;
        if (op == 0xC5) { map = 1; p += 1; }
        else if (op == 0xC4) { map = p[0] & 0x1F; p += 2; }
        else { map = p[0] & 0x07; p += 3; }
        if (map != 1 && map != 2 && map != 3) return false;
        uint8_t vop = *p++;
        const uint8_t* m = p;
        p += ModRM(p, adsz, &rip);
        if (rip >= 0) rip += (int)(m - code);
        static const uint8_t vexImm8[] = {0x70, 0x71, 0x72, 0x73, 0xC2, 0xC4, 0xC5, 0xC6};
        if (map == 3 || (map == 1 && Has(vexImm8, sizeof vexImm8, vop))) p += 1;
        out.len = p - code;
        out.ripDispOff = rip;
        return true;
    }

    if (op == 0x0F) {
        out.twoByte = true;
        uint8_t op2 = *p++;
        out.op = op2;
        if (op2 == 0x38 || op2 == 0x3A) {  // three-byte maps
            p++;
            const uint8_t* m = p;
            p += ModRM(p, adsz, &rip);
            if (rip >= 0) rip += (int)(m - code);
            if (op2 == 0x3A) p += 1;
        } else if (op2 >= 0x80 && op2 <= 0x8F) {   // jcc rel32
            out.relOff = (int)(p - code); out.relSize = 4; p += 4;
        } else {
            static const uint8_t noModrm[] = {0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0E, 0x30, 0x31, 0x32,
                                              0x33, 0x34, 0x35, 0x37, 0x77, 0xA0, 0xA1, 0xA2, 0xA8, 0xA9,
                                              0xAA, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF};
            static const uint8_t imm8[] = {0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6};
            if (!Has(noModrm, sizeof noModrm, op2)) {
                const uint8_t* m = p;
                p += ModRM(p, adsz, &rip);
                if (rip >= 0) rip += (int)(m - code);
                if (Has(imm8, sizeof imm8, op2)) p += 1;
            }
        }
        out.len = p - code;
        out.ripDispOff = rip;
        return true;
    }

    const size_t immZ = opsz ? 2 : 4;  // imm16/32
    auto modrm = [&]() {
        const uint8_t* m = p;
        p += ModRM(p, adsz, &rip);
        if (rip >= 0) rip += (int)(m - code);
    };

    uint8_t hi = op & 0xF0, lo = op & 0x0F;
    if (hi <= 0x30 && (lo <= 3 || (lo >= 8 && lo <= 0xB))) modrm();          // ALU r/m
    else if (hi <= 0x30 && (lo == 4 || lo == 0xC)) p += 1;                   // ALU al, imm8
    else if (hi <= 0x30 && (lo == 5 || lo == 0xD)) p += immZ;                // ALU eax, imm
    else if (hi <= 0x30) return false;                                      // 06/07/0E/.../27/2F/37/3F: invalid in x64
    else if (hi == 0x50) {}                                                  // push/pop reg
    else if (op == 0x63) modrm();                                            // movsxd
    else if (op == 0x68) p += immZ;
    else if (op == 0x69) { modrm(); p += immZ; }
    else if (op == 0x6A) p += 1;
    else if (op == 0x6B) { modrm(); p += 1; }
    else if (hi == 0x70) { out.relOff = (int)(p - code); out.relSize = 1; p += 1; } // jcc rel8
    else if (op == 0x80 || op == 0x82 || op == 0x83) { modrm(); p += 1; }
    else if (op == 0x81) { modrm(); p += immZ; }
    else if (op >= 0x84 && op <= 0x8F) modrm();
    else if (hi == 0x90) {}                                                  // xchg/nop/cwd/pushf...
    else if (op >= 0xA0 && op <= 0xA3) p += adsz ? 4 : 8;                    // mov moffs
    else if (op == 0xA8) p += 1;
    else if (op == 0xA9) p += immZ;
    else if (op >= 0xA4 && op <= 0xAF) {}                                    // string ops
    else if (op >= 0xB0 && op <= 0xB7) p += 1;
    else if (op >= 0xB8 && op <= 0xBF) p += rexW ? 8 : immZ;
    else if (op == 0xC0 || op == 0xC1) { modrm(); p += 1; }
    else if (op == 0xC2 || op == 0xCA) p += 2;
    else if (op == 0xC3 || op == 0xCB || op == 0xC9 || op == 0xCC || op == 0xF4) {}
    else if (op == 0xC6) { modrm(); p += 1; }
    else if (op == 0xC7) { modrm(); p += immZ; }
    else if (op == 0xC8) p += 3;
    else if (op == 0xCD) p += 1;
    else if (op >= 0xD0 && op <= 0xD3) modrm();
    else if (op >= 0xD8 && op <= 0xDF) modrm();                              // x87
    else if (op == 0xE8 || op == 0xE9) { out.relOff = (int)(p - code); out.relSize = 4; p += 4; }
    else if (op == 0xEB) { out.relOff = (int)(p - code); out.relSize = 1; p += 1; }
    else if (op >= 0xE0 && op <= 0xE3) return false;                         // loop/jrcxz: not relocatable here
    else if (op >= 0xE4 && op <= 0xE7) p += 1;
    else if (op >= 0xEC && op <= 0xEF) {}
    else if (op == 0xF5 || (op >= 0xF8 && op <= 0xFD)) {}
    else if (op == 0xF6) { uint8_t reg = (p[0] >> 3) & 7; modrm(); if (reg < 2) p += 1; }
    else if (op == 0xF7) { uint8_t reg = (p[0] >> 3) & 7; modrm(); if (reg < 2) p += immZ; }
    else if (op == 0xFE || op == 0xFF) modrm();
    else return false;

    out.len = p - code;
    out.ripDispOff = rip;
    return out.len > 0 && out.len <= 15;
}

size_t InsnLength(const uint8_t* p) {
    Insn i;
    return Decode(p, i) ? i.len : 0;
}

// ------------------------------------------------------------------ near allocation

// Executable memory within +-2GB of `target`, sub-allocated in 64-byte slots.
struct Pool { uint8_t* base; size_t used; };
static std::vector<Pool> s_pools;
static constexpr size_t kPoolSize = 0x10000;

static uint8_t* AllocNear(uintptr_t target, size_t bytes) {
    bytes = (bytes + 15) & ~size_t(15);
    for (auto& pool : s_pools) {
        intptr_t d = (intptr_t)pool.base - (intptr_t)target;
        if (d > INT32_MIN / 2 && d < INT32_MAX / 2 && pool.used + bytes <= kPoolSize) {
            uint8_t* r = pool.base + pool.used;
            pool.used += bytes;
            return r;
        }
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = target > 0x70000000 ? target - 0x70000000 : (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t hi = target + 0x70000000;
    // Search downwards first then upwards, stepping by allocation granularity.
    for (int dir = -1; dir <= 1; dir += 2) {
        uintptr_t a = (target & ~(gran - 1));
        for (;;) {
            a = dir < 0 ? a - gran : a + gran;
            if (a < lo || a > hi) break;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            if (mbi.State != MEM_FREE) continue;
            void* r = VirtualAlloc((void*)a, kPoolSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (r) {
                s_pools.push_back({(uint8_t*)r, bytes});
                return (uint8_t*)r;
            }
        }
    }
    return nullptr;
}

static void EmitAbsJmp(uint8_t*& o, uint64_t dst) {
    o[0] = 0xFF; o[1] = 0x25; *(int32_t*)(o + 2) = 0; *(uint64_t*)(o + 6) = dst;
    o += 14;
}

// ------------------------------------------------------------------ install / remove

struct HookRec {
    uint8_t  saved[32];
    size_t   stolen;
    uint8_t* tramp;
    uint8_t* relay;
};
static std::mutex s_mtx;
static std::map<void*, HookRec> s_hooks;

bool HookInstall(void* target, void* detour, void** original, std::string* err) {
    std::lock_guard<std::mutex> g(s_mtx);
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    if (s_hooks.count(target)) return fail("already hooked");

    uint8_t* t = (uint8_t*)target;
    // Decode enough whole instructions to cover the 5-byte jmp.
    std::vector<Insn> insns;
    size_t stolen = 0;
    while (stolen < 5) {
        Insn i;
        if (!Decode(t + stolen, i)) return fail("unsupported instruction in prologue");
        if (!i.twoByte && (i.op == 0xC3 || i.op == 0xC2 || i.op == 0xCC) && stolen + i.len < 5)
            return fail("function too short to hook");
        insns.push_back(i);
        stolen += i.len;
    }
    if (stolen > sizeof(HookRec::saved)) return fail("prologue too long");

    // Trampoline: relocated stolen instructions (each at most 14+2 bytes expanded) + jmp back.
    uint8_t* tramp = AllocNear((uintptr_t)t, stolen * 4 + 64);
    uint8_t* relay = AllocNear((uintptr_t)t, 16);
    if (!tramp || !relay) return fail("no free memory within 2GB of target");

    uint8_t* o = tramp;
    size_t off = 0;
    for (auto& i : insns) {
        const uint8_t* src = t + off;
        uintptr_t next = (uintptr_t)src + i.len;
        if (i.relOff >= 0) {
            int64_t rel = i.relSize == 1 ? (int8_t)src[i.relOff] : *(int32_t*)(src + i.relOff);
            uint64_t dst = next + rel;
            bool isCall = !i.twoByte && i.op == 0xE8;
            bool isJmp  = !i.twoByte && (i.op == 0xE9 || i.op == 0xEB);
            if (isCall) {           // call [rip+2]; jmp +8; dq dst
                o[0] = 0xFF; o[1] = 0x15; *(int32_t*)(o + 2) = 2; o[6] = 0xEB; o[7] = 0x08;
                *(uint64_t*)(o + 8) = dst; o += 16;
            } else if (isJmp) {
                EmitAbsJmp(o, dst);
            } else {                // jcc: inverted short jcc over an absolute jmp
                uint8_t cc = i.op & 0x0F;  // same condition nibble for 7x and 0F 8x
                o[0] = (uint8_t)(0x70 | (cc ^ 1)); o[1] = 14; o += 2;
                EmitAbsJmp(o, dst);
            }
        } else {
            memcpy(o, src, i.len);
            if (i.ripDispOff >= 0) {
                int64_t abs = (int64_t)next + *(int32_t*)(src + i.ripDispOff);
                int64_t nd = abs - (int64_t)((uintptr_t)o + i.len);
                if (nd < INT32_MIN || nd > INT32_MAX) return fail("RIP-relative operand out of range");
                *(int32_t*)(o + i.ripDispOff) = (int32_t)nd;
            }
            o += i.len;
        }
        off += i.len;
    }
    EmitAbsJmp(o, (uint64_t)(t + stolen));

    uint8_t* r = relay;
    EmitAbsJmp(r, (uint64_t)detour);

    HookRec rec{};
    memcpy(rec.saved, t, stolen);
    rec.stolen = stolen;
    rec.tramp = tramp;
    rec.relay = relay;

    DWORD oldProt;
    if (!VirtualProtect(t, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) return fail("VirtualProtect failed");
    uint8_t patch[32];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((intptr_t)relay - (intptr_t)(t + 5));
    // Publish the trampoline before the jump can be taken.
    *original = tramp;
    MemoryBarrier();
    memcpy(t, patch, stolen);
    VirtualProtect(t, stolen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);

    s_hooks[target] = rec;
    return true;
}

bool HookRemove(void* target) {
    std::lock_guard<std::mutex> g(s_mtx);
    auto it = s_hooks.find(target);
    if (it == s_hooks.end()) return false;
    DWORD oldProt;
    VirtualProtect(target, it->second.stolen, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(target, it->second.saved, it->second.stolen);
    VirtualProtect(target, it->second.stolen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, it->second.stolen);
    // Trampoline memory is intentionally leaked: another thread may still be inside it.
    s_hooks.erase(it);
    return true;
}

}  // namespace gml
