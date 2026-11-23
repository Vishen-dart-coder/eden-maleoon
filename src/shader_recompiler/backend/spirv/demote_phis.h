// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Shader::Backend::SPIRV {

// Rewrites every OpPhi into a Function-storage variable: the phi becomes an OpLoad, and each
// predecessor block stores its incoming value right before its merge/terminator instruction.
// This is the shape glslang emits, and it avoids drivers (Huawei Maleoon / Bisheng compiler)
// that hang on phi-heavy structured control flow.
// Returns false and leaves the module untouched if anything unexpected is found.
inline bool DemotePhisToVariables(std::vector<uint32_t>& code) {
    constexpr uint32_t OpTypePointer = 32;
    constexpr uint32_t OpFunction = 54;
    constexpr uint32_t OpFunctionEnd = 56;
    constexpr uint32_t OpVariable = 59;
    constexpr uint32_t OpLoad = 61;
    constexpr uint32_t OpStore = 62;
    constexpr uint32_t OpPhi = 245;
    constexpr uint32_t OpLoopMerge = 246;
    constexpr uint32_t OpSelectionMerge = 247;
    constexpr uint32_t OpLabel = 248;
    constexpr uint32_t StorageClassFunction = 7;

    const auto is_terminator = [](uint32_t op) {
        return (op >= 249 && op <= 255) || op == 4416 /*OpTerminateInvocation*/ ||
               op == 4448 /*OpIgnoreIntersectionKHR*/ || op == 4449 /*OpTerminateRayKHR*/;
    };

    if (code.size() < 5) {
        return false;
    }
    struct Inst {
        size_t offset;
        uint32_t count;
        uint32_t op;
    };
    std::vector<Inst> insts;
    for (size_t off = 5; off < code.size();) {
        const uint32_t count = code[off] >> 16;
        if (count == 0 || off + count > code.size()) {
            return false;
        }
        insts.push_back({off, count, code[off] & 0xFFFFu});
        off += count;
    }

    uint32_t bound = code[3];
    std::map<uint32_t, uint32_t> func_ptr_types; // pointee -> Function pointer type
    size_t first_function = insts.size();
    for (size_t i = 0; i < insts.size(); ++i) {
        const Inst& in = insts[i];
        if (in.op == OpFunction) {
            first_function = i;
            break;
        }
        if (in.op == OpTypePointer && in.count == 4 &&
            code[in.offset + 2] == StorageClassFunction) {
            func_ptr_types.emplace(code[in.offset + 3], code[in.offset + 1]);
        }
    }

    // Per-instruction edits
    std::unordered_map<size_t, std::vector<uint32_t>> insert_before; // inst index -> words
    std::unordered_map<size_t, std::vector<uint32_t>> insert_after;
    std::unordered_map<size_t, std::vector<uint32_t>> replace;
    std::vector<uint32_t> new_types;
    bool any_phi = false;

    size_t i = first_function;
    while (i < insts.size()) {
        if (insts[i].op != OpFunction) {
            ++i;
            continue;
        }
        // Gather blocks of this function
        size_t entry_label = 0;
        bool have_entry = false;
        std::unordered_map<uint32_t, size_t> block_end; // label id -> index to insert stores at
        std::vector<size_t> phis;
        uint32_t current_label = 0;
        size_t j = i + 1;
        for (; j < insts.size() && insts[j].op != OpFunctionEnd; ++j) {
            const Inst& in = insts[j];
            if (in.op == OpLabel) {
                current_label = code[in.offset + 1];
                if (!have_entry) {
                    entry_label = j;
                    have_entry = true;
                }
            } else if (in.op == OpPhi) {
                phis.push_back(j);
            } else if (is_terminator(in.op)) {
                size_t at = j;
                if (j > 0 && (insts[j - 1].op == OpSelectionMerge ||
                              insts[j - 1].op == OpLoopMerge)) {
                    at = j - 1;
                }
                block_end[current_label] = at;
            }
        }
        if (j >= insts.size()) {
            return false;
        }
        if (!phis.empty()) {
            if (!have_entry) {
                return false;
            }
            any_phi = true;
            std::vector<uint32_t>& vars = insert_after[entry_label];
            for (const size_t p : phis) {
                const Inst& in = insts[p];
                if (in.count < 3 || ((in.count - 3) % 2) != 0) {
                    return false;
                }
                const uint32_t type = code[in.offset + 1];
                const uint32_t result = code[in.offset + 2];
                auto [it, inserted] = func_ptr_types.try_emplace(type, 0);
                if (inserted) {
                    it->second = bound++;
                    new_types.insert(new_types.end(),
                                     {(4u << 16) | OpTypePointer, it->second,
                                      StorageClassFunction, type});
                }
                const uint32_t var = bound++;
                vars.insert(vars.end(), {(4u << 16) | OpVariable, it->second, var,
                                         StorageClassFunction});
                replace[p] = {(4u << 16) | OpLoad, type, result, var};
                for (uint32_t k = 3; k + 1 < in.count; k += 2) {
                    const uint32_t value = code[in.offset + k];
                    const uint32_t parent = code[in.offset + k + 1];
                    const auto end = block_end.find(parent);
                    if (end == block_end.end()) {
                        return false;
                    }
                    auto& stores = insert_before[end->second];
                    stores.insert(stores.end(), {(3u << 16) | OpStore, var, value});
                }
            }
        }
        i = j + 1;
    }
    if (!any_phi) {
        return true;
    }

    std::vector<uint32_t> out;
    out.reserve(code.size() + new_types.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    out[3] = bound;
    for (size_t k = 0; k < insts.size(); ++k) {
        if (k == first_function) {
            out.insert(out.end(), new_types.begin(), new_types.end());
        }
        if (const auto it = insert_before.find(k); it != insert_before.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        if (const auto it = replace.find(k); it != replace.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
        } else {
            const Inst& in = insts[k];
            out.insert(out.end(), code.begin() + in.offset, code.begin() + in.offset + in.count);
        }
        if (const auto it = insert_after.find(k); it != insert_after.end()) {
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
    }
    code = std::move(out);
    return true;
}

} // namespace Shader::Backend::SPIRV
