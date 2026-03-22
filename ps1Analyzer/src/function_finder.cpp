// ps1xAnalyzer — Function Finder Implementation
// Multi-pass function detection for PS1 MIPS binaries

#include "ps1recomp/function_finder.h"

#include <algorithm>
#include <fmt/format.h>

namespace ps1recomp {

// ─── Main Entry Point ────────────────────────────────────

void FunctionFinder::findFunctions(const ElfParser& elf) {
    m_functions.clear();
    m_jalTargets.clear();

    // Pass 1: Entry point
    addEntryPoint(elf);

    // Pass 2: ELF symbol table
    addSymbolFunctions(elf);

    // Pass 3 & 4: Heuristic scans on .text section
    const Section* text = elf.getTextSection();
    if (text != nullptr && text->data != nullptr && text->size >= 4) {
        scanJALTargets(*text);
        scanPrologues(*text);
    }

    // Pass 5: Compute sizes from sorted addresses
    if (text != nullptr) {
        computeBoundaries(*text);
    }
}

// ─── Pass 1: Entry Point ─────────────────────────────────

void FunctionFinder::addEntryPoint(const ElfParser& elf) {
    addFunction(elf.getEntryPoint(), "__start", FunctionSource::EntryPoint);
}

// ─── Pass 2: ELF Symbols ─────────────────────────────────

void FunctionFinder::addSymbolFunctions(const ElfParser& elf) {
    for (const auto& sym : elf.getSymbols()) {
        if (sym.isFunction() && sym.address != 0) {
            addFunction(sym.address, sym.name, FunctionSource::Symbol);
        }
    }
}

// ─── Pass 3: JAL Target Scan ─────────────────────────────

void FunctionFinder::scanJALTargets(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t pc = text.vaddr + (i * 4);

        if (mips::isJAL(instr)) {
            uint32_t target = mips::jalTarget(pc, instr);

            // Only accept targets within the text section
            if (text.containsAddress(target)) {
                m_jalTargets.insert(target);

                if (!hasFunction(target)) {
                    // Generate name based on address
                    std::string name = fmt::format("func_{:08X}", target);
                    addFunction(target, name, FunctionSource::JALTarget);
                }
            }
        }
    }
}

// ─── Pass 4: Prologue Pattern Scan ───────────────────────

void FunctionFinder::scanPrologues(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t addr = text.vaddr + (i * 4);

        if (mips::isStackPrologue(instr) && !hasFunction(addr)) {
            // Additional validation: check if preceded by JR $ra + delay slot
            // or by NOP padding (common between functions)
            bool likelyStart = false;

            if (i == 0) {
                // First instruction in section — likely function start
                likelyStart = true;
            } else if (i >= 2) {
                // Check if the instruction 2 slots back is JR $ra
                // (the instruction right before is the delay slot)
                uint32_t prevInstr = readInstruction(text, (i - 2) * 4);
                if (mips::isJR_RA(prevInstr)) {
                    likelyStart = true;
                }
            }

            if (i >= 1) {
                // Check if previous instruction is a NOP (padding)
                uint32_t prevInstr = readInstruction(text, (i - 1) * 4);
                if (mips::isNOP(prevInstr)) {
                    likelyStart = true;
                }
            }

            if (likelyStart) {
                std::string name = fmt::format("func_{:08X}", addr);
                addFunction(addr, name, FunctionSource::Prologue);
            }
        }
    }
}

// ─── Pass 5: Compute Boundaries ──────────────────────────

void FunctionFinder::computeBoundaries(const Section& text) {
    // Sort functions by address
    std::sort(m_functions.begin(), m_functions.end());

    // Compute sizes: each function extends to the start of the next
    for (size_t i = 0; i < m_functions.size(); ++i) {
        if (m_functions[i].size != 0) {
            continue; // Already has size from symbol table
        }

        if (i + 1 < m_functions.size()) {
            m_functions[i].size = m_functions[i + 1].address - m_functions[i].address;
        } else {
            // Last function: extends to end of text section
            uint32_t textEnd = text.vaddr + text.size;
            if (m_functions[i].address < textEnd) {
                m_functions[i].size = textEnd - m_functions[i].address;
            }
        }
    }

    // Determine leaf functions: scan each function for JAL/JALR instructions
    for (auto& func : m_functions) {
        if (func.size == 0 || !text.containsAddress(func.address)) {
            continue;
        }

        func.isLeaf = true;
        uint32_t offset = func.address - text.vaddr;
        uint32_t numInstr = func.size / 4;

        for (uint32_t i = 0; i < numInstr; ++i) {
            uint32_t instr = readInstruction(text, offset + i * 4);
            uint32_t opcode = mips::getOpcode(instr);

            if (opcode == mips::OP_JAL) {
                func.isLeaf = false;
                break;
            }
            if (opcode == mips::OP_SPECIAL && mips::getFunction(instr) == mips::FUNC_JALR) {
                func.isLeaf = false;
                break;
            }
        }
    }
}

// ─── Queries ─────────────────────────────────────────────

const FunctionInfo* FunctionFinder::findByAddress(uint32_t addr) const {
    // Binary search (m_functions is sorted)
    auto it = std::lower_bound(m_functions.begin(), m_functions.end(), addr,
        [](const FunctionInfo& f, uint32_t a) { return f.address < a; });

    if (it != m_functions.end() && it->address == addr) {
        return &(*it);
    }
    return nullptr;
}

const FunctionInfo* FunctionFinder::findContaining(uint32_t addr) const {
    if (m_functions.empty()) return nullptr;

    // Find last function with address <= addr
    auto it = std::upper_bound(m_functions.begin(), m_functions.end(), addr,
        [](uint32_t a, const FunctionInfo& f) { return a < f.address; });

    if (it == m_functions.begin()) return nullptr;
    --it;

    // Check if addr is within this function's range
    if (it->size > 0 && addr < it->address + it->size) {
        return &(*it);
    }
    // If size unknown, assume it could contain it
    if (it->size == 0) {
        return &(*it);
    }
    return nullptr;
}

// ─── Helpers ─────────────────────────────────────────────

void FunctionFinder::addFunction(uint32_t addr, const std::string& name, FunctionSource source) {
    // Don't add if already exists (prefer earlier source — higher priority)
    if (hasFunction(addr)) {
        return;
    }

    FunctionInfo info;
    info.address = addr;
    info.size = 0;      // Computed later in computeBoundaries
    info.name = name;
    info.source = source;
    info.isLeaf = false; // Determined later

    m_functions.push_back(std::move(info));
}

bool FunctionFinder::hasFunction(uint32_t addr) const {
    return std::any_of(m_functions.begin(), m_functions.end(),
        [addr](const FunctionInfo& f) { return f.address == addr; });
}

uint32_t FunctionFinder::readInstruction(const Section& sec, uint32_t offset) {
    if (offset + 4 > sec.size || sec.data == nullptr) {
        return 0;
    }
    // Little-endian read (PS1 is MIPS LE)
    const uint8_t* p = sec.data + offset;
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace ps1recomp
