/////////////////////////////////////////////////////////////////////////
//
// Author: Mateusz Jurczyk (mjurczyk@google.com)
//
// Copyright 2017-2018 Google LLC
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "common.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpu/cpu.h"

#include "logging.pb.h"

// See instrumentation.h for globals' documentation.
namespace globals {
  bochspwn_config config;
  std::vector<module_info *> modules;
  std::unordered_set<bx_address> known_callstack_item;
  uint8_t *heap_taint_alloc;
  uint8_t *stack_taint_alloc;
  bool rep_movs;
  bool esp_change;
  uint32_t esp_value;
  std::unordered_set<uint32_t> strict_checking;
  std::unordered_map<uint32_t, kmem_cache> kmem_caches;
  std::unordered_map<uint32_t, uint32_t> kmem_cache_constructor_to_size;
  std::unordered_map<uint32_t, kmem_cache> per_site_cache_reqs;
  std::unordered_map<uint32_t, alloc_request> per_site_alloc_reqs;
  bool bp_active;
  uint32_t bp_address;
  uint8_t bp_orig_byte;
}  // namespace globals

// Given a kernel-mode virtual address, returns the image base of the
// corresponding module or NULL, if one is not found. Assuming that every
// executed address belongs to a valid image at any given time, not finding
// an address should be interpreted as a signal to update the current module
// database.
module_info* find_module(bx_address item) {
  unsigned int sz = globals::modules.size();
  for (unsigned int i = 0; i < sz; i++) {
    if (globals::modules[i]->module_base <= item &&
        globals::modules[i]->module_base + globals::modules[i]->module_size > item) {
      return globals::modules[i];
    }
  }

  return NULL;
}

std::string format_hex(const std::string& data) {
  std::string output;
  char buffer[256];

  for (size_t i = 0; i < data.size(); i += 16) {
    snprintf(buffer, sizeof(buffer), "%.8x: ", i);
    output += buffer;

    for (size_t j = 0; j < 16; j++) {
      if (i + j < data.size()) {
        snprintf(buffer, sizeof(buffer), "%.2x ", (unsigned char)data[i + j]);
      } else {
        strncpy(buffer, "?? ", sizeof(buffer));
      }
      output += buffer;
    }

    for (size_t j = 0; j < 16; j++) {
      if (i + j < data.size() && data[i + j] >= 0x20 && data[i + j] <= 0x7e) {
        snprintf(buffer, sizeof(buffer), "%c", data[i + j]);
      } else {
        strncpy(buffer, ".", sizeof(buffer));
      }
      output += buffer;
    }

    output += "\n";
  }

  return output;
}

const char *translate_mem_access(bug_report_t::mem_access_type type) {
  switch (type) {
    case bug_report_t::MEM_READ: return "READ";
    case bug_report_t::MEM_WRITE: return "WRITE";
    case bug_report_t::MEM_EXEC: return "EXEC";
    case bug_report_t::MEM_RW: return "R/W";
  }
  return "INVALID";
}

void invoke_guest_int3(BX_CPU_C *pcpu) {
  // Save information about the original code, so that it can be restored when
  // the breakpoint fires.
  globals::bp_active = true;
  globals::bp_address = pcpu->gen_reg[BX_32BIT_REG_EIP].dword.erx;
  globals::bp_orig_byte = 0xcc;
  read_lin_mem(pcpu, pcpu->gen_reg[BX_32BIT_REG_EIP].dword.erx, 1, &globals::bp_orig_byte);

  // Overwrite the next instruction with an INT3, which will trigger a
  // guest breakpoint.
  write_lin_mem(pcpu, pcpu->gen_reg[BX_32BIT_REG_EIP].dword.erx, 1, (void *)"\xcc");
}

