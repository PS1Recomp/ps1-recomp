// ps1xAnalyzer — PsyQ Signature Matcher Implementation
// Identifies known PsyQ SDK functions by name, prefix matching, and
// byte-level instruction-pattern scanning (for stripped binaries).

#include "ps1recomp/psyq_signatures.h"
#include "ps1recomp/elf_parser.h"
#include "ps1recomp/function_finder.h"

#include <algorithm>
#include <cstring>

namespace ps1recomp {

// ─── Constructor ─────────────────────────────────────────

PsyQMatcher::PsyQMatcher() {
    initDatabase();
}

// ─── Database Initialization ─────────────────────────────
// 70+ known PsyQ SDK functions grouped by subsystem

void PsyQMatcher::initDatabase() {
    // ── Graphics (libgpu, libgs, libgp) ──
    reg("GsInitGraph",     PsyQSubsystem::Graphics, StubType::Stub, "Initialize graphics system");
    reg("GsDefDispBuff",   PsyQSubsystem::Graphics, StubType::Stub, "Define display buffers");
    reg("GsSortClear",     PsyQSubsystem::Graphics, StubType::Stub, "Sort clear command into OT");
    reg("GsDrawOt",        PsyQSubsystem::Graphics, StubType::Stub, "Draw ordering table");
    reg("GsSortOt",        PsyQSubsystem::Graphics, StubType::Stub, "Sort ordering table");
    reg("GsSwapDispBuff",  PsyQSubsystem::Graphics, StubType::Stub, "Swap display buffers");
    reg("GsInit3D",        PsyQSubsystem::Graphics, StubType::Stub, "Initialize 3D system");
    reg("GsSetProjection", PsyQSubsystem::Graphics, StubType::Stub, "Set projection distance");
    reg("GsMapModelingData",PsyQSubsystem::Graphics, StubType::Stub, "Map TMD model data");
    reg("GsLinkObject4",   PsyQSubsystem::Graphics, StubType::Stub, "Link object to world");
    reg("GsSortObject4",   PsyQSubsystem::Graphics, StubType::Stub, "Sort 3D object into OT");
    reg("SetDispMask",     PsyQSubsystem::Graphics, StubType::Stub, "Enable/disable display");
    reg("ResetGraph",      PsyQSubsystem::Graphics, StubType::Stub, "Reset GPU");
    reg("SetDefDrawEnv",   PsyQSubsystem::Graphics, StubType::Stub, "Set drawing environment");
    reg("SetDefDispEnv",   PsyQSubsystem::Graphics, StubType::Stub, "Set display environment");
    reg("PutDrawEnv",      PsyQSubsystem::Graphics, StubType::Stub, "Apply draw environment");
    reg("PutDispEnv",      PsyQSubsystem::Graphics, StubType::Stub, "Apply display environment");
    reg("DrawPrim",        PsyQSubsystem::Graphics, StubType::Stub, "Draw primitive");
    reg("DrawOTag",        PsyQSubsystem::Graphics, StubType::Stub, "Draw ordering table tag");
    reg("DrawOTagEnv",     PsyQSubsystem::Graphics, StubType::Stub, "Draw OT with environment");
    reg("ClearOTag",       PsyQSubsystem::Graphics, StubType::Stub, "Clear ordering table");
    reg("ClearOTagR",      PsyQSubsystem::Graphics, StubType::Stub, "Clear OT reverse");
    reg("LoadImage",       PsyQSubsystem::Graphics, StubType::Stub, "Load image to VRAM");
    reg("StoreImage",      PsyQSubsystem::Graphics, StubType::Stub, "Store VRAM image");
    reg("MoveImage",       PsyQSubsystem::Graphics, StubType::Stub, "Move VRAM block");
    reg("LoadTPage",       PsyQSubsystem::Graphics, StubType::Stub, "Load texture page");
    reg("LoadClut",        PsyQSubsystem::Graphics, StubType::Stub, "Load CLUT");
    reg("GetTPage",        PsyQSubsystem::Graphics, StubType::Stub, "Get texture page ID");
    reg("GetClut",         PsyQSubsystem::Graphics, StubType::Stub, "Get CLUT ID");
    reg("FntLoad",         PsyQSubsystem::Graphics, StubType::Stub, "Load debug font");
    reg("FntOpen",         PsyQSubsystem::Graphics, StubType::Stub, "Open font stream");
    reg("FntPrint",        PsyQSubsystem::Graphics, StubType::Skip, "Debug font print");
    reg("FntFlush",        PsyQSubsystem::Graphics, StubType::Stub, "Flush font buffer");

    // ── Sound (libspu, libsnd) ──
    reg("SpuInit",         PsyQSubsystem::Sound, StubType::Stub, "Initialize SPU");
    reg("SpuSetKey",       PsyQSubsystem::Sound, StubType::Stub, "Key on/off SPU voices");
    reg("SpuSetCommonAttr", PsyQSubsystem::Sound, StubType::Stub, "Set SPU common attributes");
    reg("SpuSetVoiceAttr", PsyQSubsystem::Sound, StubType::Stub, "Set voice attributes");
    reg("SpuSetTransferMode", PsyQSubsystem::Sound, StubType::Stub, "Set transfer mode");
    reg("SpuSetTransferStartAddr", PsyQSubsystem::Sound, StubType::Stub, "Set transfer start");
    reg("SpuWrite",        PsyQSubsystem::Sound, StubType::Stub, "Write to SPU RAM");
    reg("SpuIsTransferCompleted", PsyQSubsystem::Sound, StubType::Stub, "Check SPU transfer");
    reg("SsInit",          PsyQSubsystem::Sound, StubType::Stub, "Initialize sound system");
    reg("SsStart",         PsyQSubsystem::Sound, StubType::Stub, "Start sequence");
    reg("SsSetMVol",       PsyQSubsystem::Sound, StubType::Stub, "Set master volume");
    reg("SsSetTempo",      PsyQSubsystem::Sound, StubType::Stub, "Set sequence tempo");
    reg("SsVabOpenHead",   PsyQSubsystem::Sound, StubType::Stub, "Open VAB header");
    reg("SsVabTransBody",  PsyQSubsystem::Sound, StubType::Stub, "Transfer VAB body");
    reg("SsVabTransCompleted", PsyQSubsystem::Sound, StubType::Stub, "Check VAB transfer");
    reg("SsSeqPlay",       PsyQSubsystem::Sound, StubType::Stub, "Play SEQ sequence");
    reg("SsSeqStop",       PsyQSubsystem::Sound, StubType::Stub, "Stop SEQ sequence");

    // ── CD-ROM (libcd) ──
    reg("CdInit",          PsyQSubsystem::CDROM, StubType::Stub, "Initialize CD system");
    reg("CdRead",          PsyQSubsystem::CDROM, StubType::Stub, "Read CD sectors");
    reg("CdReadSync",      PsyQSubsystem::CDROM, StubType::Stub, "Wait for CD read");
    reg("CdSearchFile",    PsyQSubsystem::CDROM, StubType::Stub, "Search file on CD");
    reg("CdControl",       PsyQSubsystem::CDROM, StubType::Stub, "Send CD control command");
    reg("CdControlB",      PsyQSubsystem::CDROM, StubType::Stub, "CD control blocking");
    reg("CdIntToPos",      PsyQSubsystem::CDROM, StubType::Passthrough, "Convert sector to MSF");
    reg("CdPosToInt",      PsyQSubsystem::CDROM, StubType::Passthrough, "Convert MSF to sector");

    // ── Controller (libetc, libpad) ──
    reg("PadInit",         PsyQSubsystem::Controller, StubType::Stub, "Initialize pad");
    reg("PadInitDirect",   PsyQSubsystem::Controller, StubType::Stub, "Init pad direct mode");
    reg("PadRead",         PsyQSubsystem::Controller, StubType::Stub, "Read pad state");
    reg("PadStartCom",     PsyQSubsystem::Controller, StubType::Stub, "Start pad communication");
    reg("PadStopCom",      PsyQSubsystem::Controller, StubType::Stub, "Stop pad communication");

    // ── VSync/System (libetc, libapi) ──
    reg("VSync",           PsyQSubsystem::VSync, StubType::Stub, "Wait for VBlank");
    reg("VSyncCallback",   PsyQSubsystem::VSync, StubType::Stub, "Set VSync callback");
    reg("DrawSync",        PsyQSubsystem::VSync, StubType::Stub, "Wait for GPU completion");
    reg("ResetCallback",   PsyQSubsystem::VSync, StubType::Stub, "Reset system callbacks");
    reg("StopCallback",    PsyQSubsystem::VSync, StubType::Stub, "Stop callbacks");
    reg("RestartCallback", PsyQSubsystem::VSync, StubType::Stub, "Restart callbacks");
    reg("ChangeClearPAD",  PsyQSubsystem::VSync, StubType::Stub, "Change pad clear mode");

    // ── Memory (libapi, kernel) ──
    reg("InitHeap",        PsyQSubsystem::Memory, StubType::Passthrough, "Initialize heap");
    reg("malloc",          PsyQSubsystem::Memory, StubType::Passthrough, "Allocate memory");
    reg("malloc3",         PsyQSubsystem::Memory, StubType::Passthrough, "Allocate (3-heap)");
    reg("free",            PsyQSubsystem::Memory, StubType::Passthrough, "Free memory");
    reg("calloc",          PsyQSubsystem::Memory, StubType::Passthrough, "Allocate zeroed");
    reg("realloc",         PsyQSubsystem::Memory, StubType::Passthrough, "Reallocate memory");

    // ── LibC (libc) ──
    reg("printf",          PsyQSubsystem::LibC, StubType::Skip, "Debug printf");
    reg("sprintf",         PsyQSubsystem::LibC, StubType::Passthrough, "String printf");
    reg("memcpy",          PsyQSubsystem::LibC, StubType::Passthrough, "Memory copy");
    reg("memset",          PsyQSubsystem::LibC, StubType::Passthrough, "Memory set");
    reg("memmove",         PsyQSubsystem::LibC, StubType::Passthrough, "Memory move");
    reg("memcmp",          PsyQSubsystem::LibC, StubType::Passthrough, "Memory compare");
    reg("strlen",          PsyQSubsystem::LibC, StubType::Passthrough, "String length");
    reg("strcpy",          PsyQSubsystem::LibC, StubType::Passthrough, "String copy");
    reg("strncpy",         PsyQSubsystem::LibC, StubType::Passthrough, "String copy n");
    reg("strcmp",           PsyQSubsystem::LibC, StubType::Passthrough, "String compare");
    reg("strcat",          PsyQSubsystem::LibC, StubType::Passthrough, "String concat");
    reg("rand",            PsyQSubsystem::LibC, StubType::Passthrough, "Random number");
    reg("srand",           PsyQSubsystem::LibC, StubType::Passthrough, "Seed random");
    reg("abs",             PsyQSubsystem::LibC, StubType::Passthrough, "Absolute value");
    reg("labs",            PsyQSubsystem::LibC, StubType::Passthrough, "Long absolute");
    reg("atoi",            PsyQSubsystem::LibC, StubType::Passthrough, "String to int");
    reg("atol",            PsyQSubsystem::LibC, StubType::Passthrough, "String to long");
    reg("strtol",          PsyQSubsystem::LibC, StubType::Passthrough, "String to long ext");
    reg("setjmp",          PsyQSubsystem::LibC, StubType::Passthrough, "Set jump point");
    reg("longjmp",         PsyQSubsystem::LibC, StubType::Passthrough, "Long jump");

    // ── Math (libmath, libgte) ──
    reg("rsin",            PsyQSubsystem::Math, StubType::Passthrough, "Fixed-point sine");
    reg("rcos",            PsyQSubsystem::Math, StubType::Passthrough, "Fixed-point cosine");
    reg("ratan2",          PsyQSubsystem::Math, StubType::Passthrough, "Fixed-point atan2");
    reg("SquareRoot0",     PsyQSubsystem::Math, StubType::Passthrough, "Square root");
    reg("SquareRoot12",    PsyQSubsystem::Math, StubType::Passthrough, "Square root 12-bit");

    // ── GTE high-level (libgte) ──
    reg("InitGeom",        PsyQSubsystem::GTE, StubType::Stub, "Initialize geometry engine");
    reg("SetGeomOffset",   PsyQSubsystem::GTE, StubType::Stub, "Set screen offset");
    reg("SetGeomScreen",   PsyQSubsystem::GTE, StubType::Stub, "Set projection distance");
    reg("SetRotMatrix",    PsyQSubsystem::GTE, StubType::Stub, "Set rotation matrix");
    reg("SetTransMatrix",  PsyQSubsystem::GTE, StubType::Stub, "Set translation matrix");
    reg("SetLightMatrix",  PsyQSubsystem::GTE, StubType::Stub, "Set light matrix");
    reg("SetColorMatrix",  PsyQSubsystem::GTE, StubType::Stub, "Set color matrix");
    reg("SetBackColor",    PsyQSubsystem::GTE, StubType::Stub, "Set background color");
    reg("SetFarColor",     PsyQSubsystem::GTE, StubType::Stub, "Set far color");
    reg("RotTransPers",    PsyQSubsystem::GTE, StubType::Stub, "Rotate+translate+perspective");
    reg("RotTransPers3",   PsyQSubsystem::GTE, StubType::Stub, "RTP for 3 vertices");
    reg("RotTrans",        PsyQSubsystem::GTE, StubType::Stub, "Rotate+translate");
    reg("NormalClip",      PsyQSubsystem::GTE, StubType::Stub, "Normal clipping");
    reg("AverageZ3",       PsyQSubsystem::GTE, StubType::Stub, "Average Z of 3 values");
    reg("AverageZ4",       PsyQSubsystem::GTE, StubType::Stub, "Average Z of 4 values");
    reg("RotMatrix",       PsyQSubsystem::GTE, StubType::Stub, "Create rotation matrix");
    reg("TransMatrix",     PsyQSubsystem::GTE, StubType::Stub, "Create translation matrix");
    reg("MulMatrix0",      PsyQSubsystem::GTE, StubType::Stub, "Multiply matrices");
    reg("MulMatrix",       PsyQSubsystem::GTE, StubType::Stub, "Multiply matrices");
    reg("CompMatrix",      PsyQSubsystem::GTE, StubType::Stub, "Compose matrices");
    reg("ApplyMatrix",     PsyQSubsystem::GTE, StubType::Stub, "Apply matrix to vector");
    reg("ApplyMatrixLV",   PsyQSubsystem::GTE, StubType::Stub, "Apply matrix (long vector)");
    reg("OuterProduct0",   PsyQSubsystem::GTE, StubType::Stub, "Vector outer product");
    reg("OuterProduct12",  PsyQSubsystem::GTE, StubType::Stub, "Vector outer product 12-bit");
}

// ─── Registration Helper ─────────────────────────────────

void PsyQMatcher::reg(const std::string& name, PsyQSubsystem sub,
                       StubType type, const std::string& desc) {
    m_database[name] = PsyQFunction{name, sub, type, desc};
}

// ─── Main Matching ───────────────────────────────────────

void PsyQMatcher::matchFunctions(const ElfParser& elf, const FunctionFinder& finder) {
    m_matches.clear();

    // Pass 1: Exact name matching from symbols
    matchByName(elf);

    // Pass 2: Prefix-based matching for unnamed functions at PsyQ addresses
    matchByPrefix(elf, finder);

    // Pass 3: Byte-level instruction-pattern scanning (stripped binaries)
    matchByByteSignature(elf, finder);
}

// ─── Pass 1: Name Matching ───────────────────────────────

void PsyQMatcher::matchByName(const ElfParser& elf) {
    for (const auto& sym : elf.getSymbols()) {
        if (!sym.isFunction() || sym.name.empty()) continue;

        auto it = m_database.find(sym.name);
        if (it != m_database.end()) {
            PsyQMatch match;
            match.address = sym.address;
            match.name = sym.name;
            match.subsystem = it->second.subsystem;
            match.stubType = it->second.stubType;
            match.exactMatch = true;
            m_matches.push_back(std::move(match));
        }
    }
}

// ─── Pass 2: Prefix Matching ─────────────────────────────

void PsyQMatcher::matchByPrefix(const ElfParser& elf, const FunctionFinder& finder) {
    for (const auto& func : finder.getFunctions()) {
        // Skip if already matched
        bool alreadyMatched = false;
        for (const auto& m : m_matches) {
            if (m.address == func.address) {
                alreadyMatched = true;
                break;
            }
        }
        if (alreadyMatched) continue;

        // Only consider functions at PsyQ-range addresses
        if (!ElfParser::isLikelyPsyQ(func.address)) continue;

        // Check if function name (from symbol) has a PsyQ prefix
        if (func.name.empty() || func.name.substr(0, 5) == "func_") continue;

        PsyQSubsystem sub = classifySubsystem(func.name);
        if (sub != PsyQSubsystem::Other) {
            PsyQMatch match;
            match.address = func.address;
            match.name = func.name;
            match.subsystem = sub;
            match.stubType = StubType::Stub; // Default for prefix-matched
            match.exactMatch = false;
            m_matches.push_back(std::move(match));
        }
    }
}

// ─── Queries ─────────────────────────────────────────────

std::vector<const PsyQMatch*> PsyQMatcher::getStubs() const {
    std::vector<const PsyQMatch*> result;
    for (const auto& m : m_matches) {
        if (m.stubType == StubType::Stub) result.push_back(&m);
    }
    return result;
}

std::vector<const PsyQMatch*> PsyQMatcher::getSkips() const {
    std::vector<const PsyQMatch*> result;
    for (const auto& m : m_matches) {
        if (m.stubType == StubType::Skip) result.push_back(&m);
    }
    return result;
}

std::vector<const PsyQMatch*> PsyQMatcher::getPassthroughs() const {
    std::vector<const PsyQMatch*> result;
    for (const auto& m : m_matches) {
        if (m.stubType == StubType::Passthrough) result.push_back(&m);
    }
    return result;
}

bool PsyQMatcher::isKnown(const std::string& name) const {
    return m_database.count(name) > 0;
}

// ─── Subsystem Classification ────────────────────────────

PsyQSubsystem PsyQMatcher::classifySubsystem(const std::string& name) {
    if (name.empty()) return PsyQSubsystem::Other;

    // ── Exact matches first (highest priority) ──

    // VSync/System — check exact names before prefix matching
    if (name == "VSync" || name == "VSyncCallback" ||
        name == "DrawSync" || name == "ResetCallback" ||
        name == "StopCallback" || name == "RestartCallback" ||
        name == "ChangeClearPAD") {
        return PsyQSubsystem::VSync;
    }

    // Memory — exact names
    if (name == "malloc" || name == "free" || name == "calloc" ||
        name == "realloc" || name == "malloc3" || name == "InitHeap") {
        return PsyQSubsystem::Memory;
    }

    // LibC — exact names and safe prefixes
    if (name == "printf" || name == "sprintf" ||
        name.substr(0, 3) == "mem" || name.substr(0, 3) == "str" ||
        name == "rand" || name == "srand" || name == "abs" ||
        name == "labs" || name == "atoi" || name == "atol") {
        return PsyQSubsystem::LibC;
    }

    // Math — exact names
    if (name.substr(0, 4) == "rsin" || name.substr(0, 4) == "rcos" ||
        name.substr(0, 5) == "ratan" || name == "SquareRoot0" ||
        name == "SquareRoot12") {
        return PsyQSubsystem::Math;
    }

    // ── Prefix-based matches (broader, lower priority) ──

    // Sound prefixes (narrow, safe)
    if (name.substr(0, 3) == "Spu" || name.substr(0, 4) == "SPU_" ||
        name.substr(0, 2) == "Ss" || name.substr(0, 4) == "SND_") {
        return PsyQSubsystem::Sound;
    }

    // CD-ROM (narrow, safe)
    if (name.substr(0, 2) == "Cd") {
        return PsyQSubsystem::CDROM;
    }

    // Controller (narrow, safe)
    if (name.substr(0, 3) == "Pad" || name.substr(0, 3) == "Gun") {
        return PsyQSubsystem::Controller;
    }

    // Graphics prefixes (specific Gs* prefix is safest)
    if (name.substr(0, 2) == "Gs" || name.substr(0, 4) == "GPU_" ||
        name.substr(0, 3) == "Fnt" || name.substr(0, 3) == "Put" ||
        name == "ResetGraph" ||
        name == "DrawPrim" || name == "DrawOTag" || name == "DrawOTagEnv" ||
        name == "ClearOTag" || name == "ClearOTagR" ||
        name == "LoadImage" || name == "StoreImage" || name == "MoveImage" ||
        name == "LoadTPage" || name == "LoadClut" ||
        name == "GetTPage" || name == "GetClut" ||
        name == "SetDispMask" || name == "SetDefDrawEnv" || name == "SetDefDispEnv") {
        return PsyQSubsystem::Graphics;
    }

    // GTE prefixes
    if (name.substr(0, 4) == "gte_" || name.substr(0, 3) == "Rot" ||
        name.substr(0, 5) == "Trans" || name.substr(0, 3) == "Mul" ||
        name.substr(0, 4) == "Comp" || name.substr(0, 5) == "Apply" ||
        name == "InitGeom" || name == "NormalClip" ||
        name.substr(0, 7) == "Average" || name.substr(0, 5) == "Outer" ||
        name == "SetGeomOffset" || name == "SetGeomScreen" ||
        name == "SetRotMatrix" || name == "SetTransMatrix" ||
        name == "SetLightMatrix" || name == "SetColorMatrix" ||
        name == "SetBackColor" || name == "SetFarColor") {
        return PsyQSubsystem::GTE;
    }

    // Generic Sync/Callback (fallback for anything with Sync in name)
    if (name.find("Sync") != std::string::npos ||
        name.find("Callback") != std::string::npos) {
        return PsyQSubsystem::VSync;
    }

    return PsyQSubsystem::Other;
}

// ─── String Helpers ──────────────────────────────────────

const char* PsyQMatcher::subsystemName(PsyQSubsystem sub) {
    switch (sub) {
        case PsyQSubsystem::Graphics:   return "Graphics";
        case PsyQSubsystem::Sound:      return "Sound";
        case PsyQSubsystem::CDROM:      return "CD-ROM";
        case PsyQSubsystem::Controller: return "Controller";
        case PsyQSubsystem::GTE:        return "GTE";
        case PsyQSubsystem::VSync:      return "VSync";
        case PsyQSubsystem::Memory:     return "Memory";
        case PsyQSubsystem::LibC:       return "LibC";
        case PsyQSubsystem::Math:       return "Math";
        case PsyQSubsystem::Other:      return "Other";
    }
    return "Unknown";
}

const char* PsyQMatcher::stubTypeName(StubType type) {
    switch (type) {
        case StubType::Stub:        return "stub";
        case StubType::Skip:        return "skip";
        case StubType::Passthrough: return "passthrough";
        case StubType::Recompile:   return "recompile";
    }
    return "unknown";
}

// ─── Pass 3: Byte-Signature Matching ────────────────────
//
// For stripped binaries (no ELF symbols), we scan each function's first
// `scanWindow` instructions looking for characteristic MIPS opcode sequences
// that uniquely identify PsyQ SDK functions.
//
// Each signature is a vector of (pattern, mask) instruction pairs that must
// appear CONSECUTIVELY somewhere in the scan window.  The mask covers which
// bits of each 32-bit instruction must match exactly.
//
// Encoding notes (MIPS I, big-endian bit numbering in the spec):
//   I-type: opcode[31:26] rs[25:21] rt[20:16] imm[15:0]
//   R-type: opcode(000000) rs rt rd shamt funct[5:0]
//   J-type: opcode[31:26] target[25:0]
//
// Hardware register constants (fixed across all PS1 games):
//   0x1F80_1810  GPU GP0 (data/command port)
//   0x1F80_1814  GPU GP1 (status/control port)
//   0x1F80_1800  CDROM index register
//   0x1F80_1D80  SPU master volume left
//   0x1F80_1D82  SPU master volume right
//
// BIOS call convention (PsyQ 3.x):
//   addiu t1, zero, FUNC_IDX   ; 0x24090000 | IDX
//   lui   at, 0                ; 0x3C010000
//   addiu at, at, 0xA0         ; 0x242100A0  (for A0 table)
//   jr    at                   ; 0x00200008
//   nop                        ; 0x00000000

const std::vector<PsyQMatcher::ByteSig>& PsyQMatcher::getSignatures() {
    using InstrPattern = PsyQMatcher::InstrPattern;
    using ByteSig      = PsyQMatcher::ByteSig;

    // ── MIPS encoding helpers ──────────────────────────────────────────────
    // Masks: all-ones = exact match; 0xFFFF0000 = match opcode+regs, ignore imm
    constexpr uint32_t MASK_EXACT   = 0xFFFFFFFFu;
    constexpr uint32_t MASK_IMM     = 0xFFFF0000u; // match opcode+regs, ignore imm

    // ── Common instruction patterns ───────────────────────────────────────
    // lui at, 0 — load upper zero (part of BIOS call prelude)
    const InstrPattern LUI_AT_0        = {0x3C010000u, MASK_EXACT};
    // addiu at, at, 0xA0 — complete A0 BIOS call address
    const InstrPattern ADDIU_AT_A0     = {0x242100A0u, MASK_EXACT};
    // addiu at, at, 0xB0 — complete B0 BIOS call address
    const InstrPattern ADDIU_AT_B0     = {0x242100B0u, MASK_EXACT};
    // jr at — jump to register at (0x00200008)
    const InstrPattern JR_AT           = {0x00200008u, MASK_EXACT};
    // lui at, 0x1F80 — load hardware base address into at
    const InstrPattern LUI_AT_HW       = {0x3C011F80u, MASK_EXACT};
    // lui v0, 0x1F80 — load hardware base address into v0
    const InstrPattern LUI_V0_HW       = {0x3C021F80u, MASK_EXACT};

    // Helper: addiu t1, zero, N — set BIOS function index N
    auto BIOS_IDX = [](uint32_t n) -> InstrPattern {
        return {0x24090000u | (n & 0xFFFFu), MASK_EXACT};
    };
    // Helper: sw zero, offset(at) — store 0 to hardware register via at
    auto SW_ZERO_AT = [](uint16_t offset) -> InstrPattern {
        return {0xAC200000u | offset, MASK_EXACT};
    };
    // Helper: sh zero, offset(at) — store halfword 0 to hardware reg via at
    auto SH_ZERO_AT = [](uint16_t offset) -> InstrPattern {
        return {0xA4200000u | offset, MASK_EXACT};
    };
    // Helper: sb zero, offset(at) — store byte 0 to hardware reg via at
    auto SB_ZERO_AT = [](uint16_t offset) -> InstrPattern {
        return {0xA0200000u | offset, MASK_EXACT};
    };

    static std::vector<ByteSig> sigs;
    if (!sigs.empty()) return sigs;

    // ─── SpuInit ────────────────────────────────────────────────────────
    // PsyQ SpuInit clears master volume registers 0x1F801D80/0x1F801D82
    // and enables the SPU by writing to control register 0x1F801DAA.
    // Characteristic: lui at/v0, 0x1F80 + sh zero, 0x1D80(reg) (MVOL_L=0)
    {
        ByteSig s;
        s.name        = "SpuInit";
        s.scanWindow  = 48;
        s.instrs = {
            LUI_AT_HW,
            SH_ZERO_AT(0x1D80u),   // SPU MVOL_L = 0
        };
        sigs.push_back(std::move(s));
    }
    // Variant using v0 instead of at
    {
        ByteSig s;
        s.name        = "SpuInit";
        s.scanWindow  = 48;
        s.instrs = {
            LUI_V0_HW,
            // sh zero, 0x1D80(v0): opcode(101000) rs(v0=2) rt(0) imm(0x1D80)
            {0xA4401D80u, MASK_EXACT},
        };
        sigs.push_back(std::move(s));
    }

    // ─── ResetGraph ─────────────────────────────────────────────────────
    // PsyQ ResetGraph sends GP1 command 0x00000000 (GPU reset) to 0x1F801814.
    // Unique: lui at, 0x1F80 + sw zero, 0x1814(at)
    {
        ByteSig s;
        s.name        = "ResetGraph";
        s.scanWindow  = 32;
        s.instrs = {
            LUI_AT_HW,
            SW_ZERO_AT(0x1814u),   // GP1[0x00000000] = GPU reset
        };
        sigs.push_back(std::move(s));
    }

    // ─── CdInit ─────────────────────────────────────────────────────────
    // PsyQ CdInit calls A0:0xAD (_96_CdReset) to initialize the hardware.
    // Sequence: addiu t1, zero, 0xAD + lui at, 0 + addiu at, at, 0xA0 + jr at
    {
        ByteSig s;
        s.name        = "CdInit";
        s.scanWindow  = 64;
        s.instrs = {
            BIOS_IDX(0xADu),   // addiu t1, zero, 0xAD
            LUI_AT_0,          // lui at, 0
            ADDIU_AT_A0,       // addiu at, at, 0xA0
            JR_AT,             // jr at
        };
        sigs.push_back(std::move(s));
    }

    // ─── VSync (simple variant) ─────────────────────────────────────────
    // PsyQ VSync calls B0:0x03 (VSync/WaitVbl) to block until VBlank.
    // Some PsyQ versions use B0:0x03; others call the counter directly.
    {
        ByteSig s;
        s.name        = "VSync";
        s.scanWindow  = 48;
        s.instrs = {
            BIOS_IDX(0x03u),   // addiu t1, zero, 0x03
            LUI_AT_0,          // lui at, 0
            ADDIU_AT_B0,       // addiu at, at, 0xB0
            JR_AT,             // jr at
        };
        sigs.push_back(std::move(s));
    }

    // ─── DrawSync ───────────────────────────────────────────────────────
    // PsyQ DrawSync(0) calls B0:0x02 (DrawSyncCallback) in some versions.
    // Most implementations poll the OT status flag directly, but the
    // B0 callback variant is detectable.
    {
        ByteSig s;
        s.name        = "DrawSync";
        s.scanWindow  = 48;
        s.instrs = {
            BIOS_IDX(0x02u),   // addiu t1, zero, 0x02
            LUI_AT_0,          // lui at, 0
            ADDIU_AT_B0,       // addiu at, at, 0xB0
            JR_AT,             // jr at
        };
        sigs.push_back(std::move(s));
    }

    // ─── InitPAD (BIOS B0:0x12 call) ───────────────────────────────────
    {
        ByteSig s;
        s.name        = "PadInit";
        s.scanWindow  = 48;
        s.instrs = {
            BIOS_IDX(0x12u),   // addiu t1, zero, 0x12 (InitPAD)
            LUI_AT_0,
            ADDIU_AT_B0,
            JR_AT,
        };
        sigs.push_back(std::move(s));
    }

    return sigs;
}

// ── Scan helpers ──────────────────────────────────────────────────────────

// Read a 4-byte LE word from raw bytes (safe bounds-checked helper)
static uint32_t readLE32(const uint8_t* data, size_t offset, size_t size) {
    if (offset + 4 > size) return 0;
    uint32_t v;
    std::memcpy(&v, data + offset, 4);
    // Convert from little-endian to host order
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

// Check whether `instrs` appear consecutively starting at instruction index
// `startInstr` within [data, data+dataSize).
static bool matchConsecutive(const std::vector<PsyQMatcher::InstrPattern>& instrs,
                              const uint8_t* data, size_t dataSize,
                              size_t startInstr) {
    for (size_t i = 0; i < instrs.size(); ++i) {
        size_t byteOffset = (startInstr + i) * 4;
        uint32_t word = readLE32(data, byteOffset, dataSize);
        if ((word & instrs[i].mask) != (instrs[i].pattern & instrs[i].mask))
            return false;
    }
    return true;
}

void PsyQMatcher::matchByByteSignature(const ElfParser& elf,
                                        const FunctionFinder& finder) {
    const auto& signatures = getSignatures();
    if (signatures.empty()) return;

    for (const auto& func : finder.getFunctions()) {
        // Skip functions already matched by Pass 1 or Pass 2
        bool already = false;
        for (const auto& m : m_matches) {
            if (m.address == func.address) { already = true; break; }
        }
        if (already) continue;

        // Locate the section containing this function
        const Section* sec = elf.findSectionByAddress(func.address);
        if (!sec || sec->type != SectionType::Text || !sec->data) continue;

        size_t funcOffset = func.address - sec->vaddr;
        size_t funcSize   = (func.size > 0) ? func.size : sec->size - funcOffset;
        if (funcOffset >= sec->size) continue;

        // Clamp available bytes to what's in the section
        size_t avail = sec->size - funcOffset;

        for (const auto& sig : signatures) {
            // Already matched this function (earlier sig or pass)
            bool skip = false;
            for (const auto& m : m_matches) {
                if (m.address == func.address) { skip = true; break; }
            }
            if (skip) break;

            // Also skip if we already have a match with this name at a
            // different address (avoid duplicates for multi-variant sigs)
            // — we allow multi-address matches if names differ.

            if (sig.instrs.empty()) continue;

            // Scan the first min(scanWindow, funcSize) instructions
            size_t windowInstrs = std::min((size_t)sig.scanWindow,
                                           std::min(funcSize, avail) / 4);
            if (windowInstrs < sig.instrs.size()) continue;

            bool found = false;
            for (size_t i = 0; i + sig.instrs.size() <= windowInstrs; ++i) {
                if (matchConsecutive(sig.instrs,
                                     sec->data + funcOffset, avail, i)) {
                    found = true;
                    break;
                }
            }

            if (found) {
                // Only add if not already matched
                auto it = m_database.find(sig.name);
                if (it != m_database.end()) {
                    PsyQMatch match;
                    match.address    = func.address;
                    match.name       = sig.name;
                    match.subsystem  = it->second.subsystem;
                    match.stubType   = it->second.stubType;
                    match.exactMatch = false;
                    m_matches.push_back(std::move(match));
                }
            }
        }
    }
}

} // namespace ps1recomp
