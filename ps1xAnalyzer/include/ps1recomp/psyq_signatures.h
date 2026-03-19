#pragma once

// ps1xAnalyzer — PsyQ Signature Matcher
// Identifies known PsyQ SDK functions for stub generation

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace ps1recomp {

// Forward declarations
class ElfParser;
class FunctionFinder;
struct FunctionInfo;

// ─── PsyQ Subsystems ────────────────────────────────────

enum class PsyQSubsystem {
    Graphics,    // Gs*, GPU_*, Draw*, Set*, Clear*
    Sound,       // Sp*, SPU_*, Spu*
    CDROM,       // Cd*, CdInit, CdRead
    Controller,  // Pad*, Gun*
    GTE,         // gte_*, Rot*, Trans*
    VSync,       // VSync, DrawSync, *Sync
    Memory,      // malloc, free, InitHeap
    LibC,        // printf, memcpy, strlen, etc.
    Math,        // rsin, rcos, ratan, SquareRoot0
    Other        // Unclassified PsyQ functions
};

// ─── Stub Types ─────────────────────────────────────────

enum class StubType {
    Stub,         // Needs runtime implementation (GPU, SPU, etc.)
    Skip,         // Can be safely skipped (debug prints, profiling)
    Passthrough,  // Use host equivalent (memcpy, malloc, etc.)
    Recompile     // Should be recompiled normally (not a known PsyQ func)
};

// ─── PsyQ Function Definition ──────────────────────────

struct PsyQFunction {
    std::string    name;
    PsyQSubsystem subsystem;
    StubType       stubType;
    std::string    description;  // Brief description for config output
};

// ─── PsyQ Match Result ──────────────────────────────────

struct PsyQMatch {
    uint32_t       address;     // Function address in binary
    std::string    name;        // Function name
    PsyQSubsystem subsystem;
    StubType       stubType;
    bool           exactMatch;  // true = name from DB, false = prefix heuristic
};

// ─── PsyQ Matcher ───────────────────────────────────────

class PsyQMatcher {
public:
    PsyQMatcher();

    /// Run matching against parsed ELF symbols and detected functions.
    void matchFunctions(const ElfParser& elf, const FunctionFinder& finder);

    /// Get all matched PsyQ functions.
    const std::vector<PsyQMatch>& getMatches() const { return m_matches; }

    /// Get functions that need runtime stubs.
    std::vector<const PsyQMatch*> getStubs() const;

    /// Get functions that can be skipped.
    std::vector<const PsyQMatch*> getSkips() const;

    /// Get functions that use host passthrough.
    std::vector<const PsyQMatch*> getPassthroughs() const;

    /// Get match count.
    size_t getMatchCount() const { return m_matches.size(); }

    /// Get the built-in database size.
    size_t getDatabaseSize() const { return m_database.size(); }

    /// Check if a function name is in the PsyQ database.
    bool isKnown(const std::string& name) const;

    /// Classify subsystem from function name (static, for external use).
    static PsyQSubsystem classifySubsystem(const std::string& name);

    /// Get human-readable subsystem name.
    static const char* subsystemName(PsyQSubsystem sub);

    /// Get human-readable stub type name.
    static const char* stubTypeName(StubType type);

private:
    std::vector<PsyQMatch> m_matches;
    std::unordered_map<std::string, PsyQFunction> m_database;

    void initDatabase();
    void matchByName(const ElfParser& elf);
    void matchByPrefix(const ElfParser& elf, const FunctionFinder& finder);

    // Database registration helper
    void reg(const std::string& name, PsyQSubsystem sub, StubType type,
             const std::string& desc = "");
};

} // namespace ps1recomp
