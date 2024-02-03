/* Rapid Silicon Copyright 2023
 */
/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

/*
  This piece of code extract important information from RTLIL::Design class
  directly. These important information includes:
    a. Number of OCLA instances being instantiated (if there is)
    b. Number of OCLA Debug Subsystem instances being instantiated (if there is)
        - this must be 1 instance
        - OCLA instance(s) must be instantiated by OCLA Debug Subsystem
    c. Each signals that user would like to probe/debug
    d. Memory Depth of the buffer to store raw data
    e. Base address of each OCLA instance
    f. a lot more
*/
/*
  Author: Chai, Chung Shien
*/

#include <string>

#include "kernel/cellaigs.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN
#define USE_LOCAL_GEN_STRING 0
#define MAXIMUM_SUPPORTED_PROBE_CORE 15
#define AXILite_SINGLE_BUS_SIGNALS 152
#define AXI4_SINGLE_BUS_SIGNALS 250

#if USE_LOCAL_GEN_STRING
std::string gen_string(const char* format_string, ...) {
  char* buf = nullptr;
  size_t bufsize = 512;
  std::string string = "";
  va_list args;
  while (1) {
    buf = new char[bufsize + 1]();
    memset(buf, 0, bufsize + 1);
    va_start(args, format_string);
    size_t n = std::vsnprintf(buf, bufsize + 1, format_string, args);
    va_end(args);
    if (n <= bufsize) {
      string.resize(n);
      memcpy((char*)(&string[0]), buf, n);
      break;
    }
    delete[] buf;
    buf = nullptr;
    bufsize *= 2;
    if (bufsize > 8192) {
      break;
    }
  }
  if (buf != nullptr) {
    delete[] buf;
  }
  return string;
}
#else
#define gen_string(...) stringf(__VA_ARGS__)
#endif

void post_msg(std::ofstream& json, int space, std::string string) {
  json << "    \"";
  while (space) {
    json << "  ";
    space--;
  }
  for (auto& c : string) {
    if (c == '\\') {
      json << '\\';
    }
    json << c;
  }
  json << "\",\n";
  json.flush();
}

#if USE_LOCAL_GEN_STRING
#define JSON_POST_MSG(space, ...) \
  { post_msg(json, space, gen_string(__VA_ARGS__)); }
#else
#define JSON_POST_MSG(space, ...) \
  { post_msg(json, space, stringf(__VA_ARGS__)); }
#endif

enum PARAM_TYPE { PARAM_IS_UINT32, PARAM_IS_UINT64, PARAM_IS_STR };

/*
  Storing module parameter information in this struct
    a. Parameter can be either uint32_t, uint64_t, std::string (determined by
       type)
    b. Parameter can only be assigned once (determined by is_assigned)
    c. The pointer ptr will be casted accordingly
*/
struct PARAM_INFO {
  PARAM_INFO(void* p = nullptr, PARAM_TYPE t = PARAM_IS_UINT32)
      : type(t), ptr(p) {}
  PARAM_TYPE type = PARAM_IS_UINT32;
  bool is_assigned = false;
  void* ptr = nullptr;
};

/*
  Storing the information about Probe to Core Mapping
*/
struct PROBE2CORE_PARAM_INFO {
  PROBE2CORE_PARAM_INFO(uint32_t c = 0, uint32_t o = 0) : core(c), offset(o) {}
  uint32_t core = 0;
  uint32_t offset = 0;
};

/*
  This struct store the information of the signal. The information include:
    a. name
    b. width size
    c. offset index
  Example of signals include:
    a. signals that user want to probe/debug
    b. signals that user want to use as trigger inputs (not supported, might
       remove)
*/
struct OCLA_SIGNAL {
  OCLA_SIGNAL(std::string f, std::string n, uint32_t w, uint32_t o, bool s)
      : fullname(f), name(n), width(w), offset(o), show_index(s) {
    size_t index = name.rfind(".");
    if (index != std::string::npos) {
      name = name.substr(index + 1);
    }
    if (name.size() > 0 && name[0] == '\\') {
      name = name.substr(1);
    }
    log_assert(width);
  }
  OCLA_SIGNAL(std::string signal, uint32_t w, uint32_t i, bool no_extra_index)
      : width(w) {
    log_assert(width);
    show_index = width > 1;
    fullname =
        gen_string("%s%s", signal.c_str(),
                   no_extra_index ? "" : gen_string("_%d", i + 1).c_str());
    name = fullname;
  }
  std::string fullname = "";
  std::string name = "";
  uint32_t width = 0;
  uint32_t offset = 0;
  bool show_index = false;
};

/*
  This is base structure of the IP
  There will be two IPs that we need to detect:
    a. OCLA IP
    b. OCLA Debug Subsystem IP
  Both IP will always has IP_TYPE, IP_VERSION and IP_ID parameter
*/
struct MODULE_IP {
  MODULE_IP(std::string n = "") : name(n) {
    log_assert(name.size());
    params["\\IP_TYPE"] = PARAM_INFO(&type, PARAM_IS_STR);
    params["\\IP_VERSION"] = PARAM_INFO(&version);
    params["\\IP_ID"] = PARAM_INFO(&id);
  }
  /*
    Set the parameter information
  */
  bool set_param(std::ofstream& json, std::string name, std::string value) {
    bool status = true;
    auto iter = params.find(name);
    if (iter == params.end()) {
      JSON_POST_MSG(1, "Ignore param %s", name.c_str());
      goto SET_PARAM_END;
    }
    if (params[name].is_assigned) {
      JSON_POST_MSG(1, "Error: Param %s had been assigned", name.c_str());
      goto SET_PARAM_ERROR;
    }
    log_assert(params[name].ptr != nullptr);
    if (params[name].type == PARAM_IS_STR) {
      if (value.size() >= 2 && value[0] == '"' &&
          value[value.size() - 1] == '"') {
        std::string* string_ptr = (std::string*)(params[name].ptr);
        (*string_ptr) = value.substr(1, value.size() - 2);
        JSON_POST_MSG(1, "Param %s - %s", name.c_str(), string_ptr->c_str());
      } else {
        JSON_POST_MSG(1,
                      "Error: Param %s value %s does not follow string format",
                      name.c_str(), value.c_str());
        goto SET_PARAM_ERROR;
      }
    } else {
      log_assert(params[name].type == PARAM_IS_UINT32 ||
                 params[name].type == PARAM_IS_UINT64);
      uint64_t u64 = 0;
      size_t index = value.find("'");
      if (index != std::string::npos) {
        std::string bit_size_str = value.substr(0, index);
        std::string bin_value = value.substr(index + 1);
        if (bit_size_str.find_first_not_of("1234567890") != std::string::npos ||
            bin_value.find_first_not_of("10") != std::string::npos) {
          JSON_POST_MSG(1,
                        "Error: Param %s value %s does not follow binary "
                        "format ({bit size string}'{binary sring})",
                        name.c_str(), value.c_str());
          goto SET_PARAM_ERROR;
        }
        uint32_t bit_size = stol(bit_size_str);
        if (bit_size == 0 || bit_size != (uint32_t)(bin_value.size())) {
          JSON_POST_MSG(1,
                        "Error: Param %s value %s does not valid bit size and "
                        "binary string size",
                        name.c_str(), value.c_str());
          goto SET_PARAM_ERROR;
        }
        if (params[name].type == PARAM_IS_UINT32) {
          if (bit_size > 32) {
            JSON_POST_MSG(1, "Error: Param uint32_t %s value %s exceed 32 bits",
                          name.c_str(), value.c_str());
            goto SET_PARAM_ERROR;
          }
        } else {
          if (bit_size > 64) {
            JSON_POST_MSG(1, "Error: Param uint64_t %s value %s exceed 64 bits",
                          name.c_str(), value.c_str());
            goto SET_PARAM_ERROR;
          }
        }
        u64 = stoll(bin_value, nullptr, 2);
      } else {
        if (value.find_first_not_of("1234567890") != std::string::npos) {
          JSON_POST_MSG(1,
                        "Error: Param %s value %s does not follow decimal "
                        "format ({decimal sring})",
                        name.c_str(), value.c_str());
          goto SET_PARAM_ERROR;
        }
        u64 = (uint64_t)(stoll(value));
      }
      if (params[name].type == PARAM_IS_UINT32) {
        uint32_t* number_ptr = (uint32_t*)(params[name].ptr);
        (*number_ptr) = (uint32_t)(u64);
        JSON_POST_MSG(1, "Param %s - %u (0x%08X)", name.c_str(), (*number_ptr),
                      (*number_ptr));
      } else {
        uint64_t* number_ptr = (uint64_t*)(params[name].ptr);
        (*number_ptr) = u64;
        JSON_POST_MSG(1, "Param %s - %lu (0x%016lX)", name.c_str(),
                      (*number_ptr), (*number_ptr));
      }
    }
    params[name].is_assigned = true;
    goto SET_PARAM_END;
  SET_PARAM_ERROR:
    status = false;
  SET_PARAM_END:
    return status;
  }
  bool check_all_params(std::ofstream& json) {
    bool all_assigned = true;
    for (auto& p : params) {
      if (!p.second.is_assigned) {
        all_assigned = false;
        JSON_POST_MSG(1, "Error: missing parameter %s", p.first.c_str());
      }
    }
    return all_assigned;
  }
  std::string name = "";
  std::map<std::string, PARAM_INFO> params;
  std::string type = "";
  uint32_t version = 0;
  uint32_t id = 0;
};

/*
  OCLA IP derived MODULE_IP
    Beside the 3 essential paramters, thiss IP need more parameter information
*/
struct OCLA_MODULE : MODULE_IP {
  OCLA_MODULE(std::string n) : MODULE_IP(n) {
    params["\\AXI_ADDR_WIDTH"] = PARAM_INFO(&axi_addr_width);
    params["\\AXI_DATA_WIDTH"] = PARAM_INFO(&axi_data_width);
    params["\\MEM_DEPTH"] = PARAM_INFO(&mem_depth);
    params["\\NO_OF_PROBES"] = PARAM_INFO(&probes_count);
    params["\\INDEX"] = PARAM_INFO(&index);
  }
  /*
    This function to determine if the detected parameter meet the requirement
  */
  bool check_type(std::ofstream& json) {
    bool status = false;
    if (type == "OCLA" && mem_depth > 0 && probes_count > 0) {
      status = true;
    } else {
      JSON_POST_MSG(1, "Error: Fail to validate parameters");
      JSON_POST_MSG(2, "IP_TYPE: %s", type.c_str());
      JSON_POST_MSG(2, "MEM_DEPTH: %d", mem_depth);
      JSON_POST_MSG(2, "NO_OF_PROBES: %d", probes_count);
    }
    return status;
  }
  /*
    Validate if all the information that we extract is good
  */
  bool finalize(std::ofstream& json, uint32_t* probe_widths, int space) {
    uint32_t total_s = 0;
    size_t probe_index = 0;
    bool status = true;
    // probes
    for (auto& s : probes) {
      total_s += s.width;
    }
    if (total_s != probes_count) {
      JSON_POST_MSG(space,
                    "Error: OCLA module at INDEX=%d has invalid total probe "
                    "signal(s) bus size %d (NO_OF_PROBES %d)",
                    index, total_s, probes_count);
      goto FINALIZE_ERROR;
    }
    if (is_axi) {
      goto FINALIZE_END;
    }
    JSON_POST_MSG(space, "Checking signal alignment");
    for (auto p : probe_order) {
      uint32_t probe_width = probe_widths[p];
      JSON_POST_MSG(space + 1,
                    "OCLA Module at INDEX=%d should have signals that aligned "
                    "with number %d, starting at signal #%ld",
                    index, probe_width, probe_index);
      while (probe_width) {
        if (probe_index >= probes.size()) {
          JSON_POST_MSG(space + 2,
                        "Does not have enough signal for the checking");
          goto FINALIZE_ERROR;
        }
        if (probes[probe_index].width > probe_width) {
          JSON_POST_MSG(space + 2, "Signal %s exceed boundary",
                        probes[probe_index].fullname.c_str());
          goto FINALIZE_ERROR;
        }
        probe_width -= probes[probe_index].width;
        probe_index++;
      }
    }
    if (probe_index != probes.size()) {
      JSON_POST_MSG(space + 1,
                    "The checking not able to cover all signal. Total signal "
                    "count=%ld, but only cover %ld",
                    probes.size(), probe_index);
      goto FINALIZE_ERROR;
    }
    goto FINALIZE_END;
  FINALIZE_ERROR:
    status = false;
  FINALIZE_END:
    return status;
  }
  bool is_axi = false;
  uint32_t axi_addr_width = 0;
  uint32_t axi_data_width = 0;
  uint32_t mem_depth = 0;
  uint32_t probes_count = 0;
  uint32_t index = 0;
  uint32_t base_address = 0;
  std::vector<OCLA_SIGNAL> probes;
  std::vector<uint32_t> probe_order;
};

/*
  OCLA Debug Subsystem IP derived MODULE_IP
    Beside the 3 essential paramters, this IP need more parameter information
*/
struct OCLA_DEBUG_SUBSYSTEM_MODULE : MODULE_IP {
  OCLA_DEBUG_SUBSYSTEM_MODULE(std::string n) : MODULE_IP(n) {
    params["\\Mode"] = PARAM_INFO(&mode, PARAM_IS_STR);
    params["\\Axi_Type"] = PARAM_INFO(&axi_type, PARAM_IS_STR);
    params["\\Sampling_Clk"] = PARAM_INFO(&sampling_clk, PARAM_IS_STR);
    params["\\Cores"] = PARAM_INFO(&cores);
    params["\\No_Probes"] = PARAM_INFO(&no_probes);
    params["\\No_AXI_Bus"] = PARAM_INFO(&no_axi_bus);
    params["\\Probes_Sum"] = PARAM_INFO(&probes_sum);
    params["\\AXI_Core_BaseAddress"] = PARAM_INFO(&axi_core_address);
    for (uint32_t i = 0; i < MAXIMUM_SUPPORTED_PROBE_CORE; i++) {
      params[gen_string("\\Probe%02d_Width", i + 1)] =
          PARAM_INFO(&ip_probe_width[i]);
      params[gen_string("\\IF%02d_BaseAddress", i + 1)] =
          PARAM_INFO(&ip_address[i]);
      params[gen_string("\\IF%02d_Probes", i + 1)] =
          PARAM_INFO(&ip_probe_info[i], PARAM_IS_UINT64);
    }
  }
  /*
    This function to determine if the detected parameter meet the requirement
  */
  bool check_type(std::ofstream& json) {
    bool status = false;
    if (type == "OCLA") {
      if (mode == "NATIVE") {
        if (no_probes > 0 && cores > 0 && no_probes >= cores &&
            cores <= MAXIMUM_SUPPORTED_PROBE_CORE) {
          status = true;
        }
      } else if (mode == "AXI") {
        if (no_probes == 0 && cores == 1 &&
            (axi_type == "AXI4" || axi_type == "AXILite") && no_axi_bus > 0 &&
            no_axi_bus <= 4) {
          status = true;
        }
      } else if (mode == "NATIVE_AXI") {
        if (no_probes > 0 && cores > 1 && no_probes >= (cores - 1) &&
            cores <= MAXIMUM_SUPPORTED_PROBE_CORE &&
            (axi_type == "AXI4" || axi_type == "AXILite") && no_axi_bus > 0 &&
            no_axi_bus <= 4) {
          status = true;
        }
      }
    }
    if (!status) {
      JSON_POST_MSG(1, "Error: Fail to validate parameters");
      JSON_POST_MSG(2, "IP_TYPE: %s", type.c_str());
      JSON_POST_MSG(2, "Mode: %s", mode.c_str());
      if (mode == "AXI" || mode == "NATIVE_AXI") {
        JSON_POST_MSG(2, "Axi_Type: %s", axi_type.c_str());
        JSON_POST_MSG(2, "No_AXI_Bus: %d", no_axi_bus);
      }
      JSON_POST_MSG(2, "Cores: %d", cores);
      JSON_POST_MSG(2, "No_Probes: %d", no_probes);
    }
    return status;
  }
  /*
    This function to determine map between probe and core
  */
  bool map_probe_core(std::ofstream& json,
                      const std::vector<OCLA_MODULE*>& ocla_modules) {
    bool status = true;
    JSON_POST_MSG(2,
                  "OCLA module should start with zero-knowledge about probe "
                  "mapping and width");
    for (auto& m : ocla_modules) {
      if (m->probe_order.size() > 0 || calculated_ip_core_width[m->index] > 0) {
        JSON_POST_MSG(3,
                      "Error: OCLA module at INDEX=%d start with probe mapping",
                      m->index);
        status = false;
      }
    }
    if (status && (mode == "NATIVE" || mode == "NATIVE_AXI")) {
      JSON_POST_MSG(2, "IF{x}_Probes must be valid");
      uint32_t native_core = mode == "NATIVE" ? cores : cores - 1;
      uint32_t native_probe = 0;
      if (native_core > 0 && native_core <= MAXIMUM_SUPPORTED_PROBE_CORE) {
        uint32_t mapping = 0;
        for (uint32_t i = 0; i < MAXIMUM_SUPPORTED_PROBE_CORE && status; i++) {
          if (i < native_core) {
            if (ip_probe_info[i] == 0) {
              JSON_POST_MSG(
                  3, "Error: IF%02d_Probes should not be null, but found it is",
                  i + 1);
              status = false;
              break;
            }
            uint64_t info = ip_probe_info[i];
            uint32_t index = 0;
            while (info) {
              uint32_t probe = (uint32_t)(info & 0xF);
              if (probe > 0 && probe <= MAXIMUM_SUPPORTED_PROBE_CORE &&
                  probe <= no_probes) {
                probe--;
                if (mapping & (1 << probe)) {
                  JSON_POST_MSG(3,
                                "Error: Duplicated Probe detected at index %d "
                                "of IF%02d_Probes=0x%016lX {Probe=%d}",
                                index, i + 1, ip_probe_info[i], probe + 1);
                  status = false;
                  break;
                }
                if (ip_probe_width[probe] == 0) {
                  JSON_POST_MSG(3,
                                "Error: Expect Probe%02d_Width to be none-zero "
                                "because of index %d of IF%02d_Probes=0x%016lX "
                                "{Probe=%d}, but it is not",
                                probe + 1, index, i + 1, ip_probe_info[i],
                                probe + 1);
                  status = false;
                  break;
                }
                ocla_modules[i]->probe_order.push_back(probe);
                probe_to_core_map[probe].core = i;
                probe_to_core_map[probe].offset = calculated_ip_core_width[i];
                calculated_ip_core_width[i] += ip_probe_width[probe];
                mapping |= (1 << probe);
                native_probe++;
              } else {
                JSON_POST_MSG(3,
                              "Error: Invalid Probe detected at index %d of "
                              "IF%02d_Probes=0x%016lX {%d}",
                              index, i + 1, ip_probe_info[i], probe);
                status = false;
                break;
              }
              info >>= 4;
              index++;
            }
          } else {
            if (ip_probe_info[i] != 0) {
              JSON_POST_MSG(
                  3, "Error: IF%02d_Probes should be null, but found 0x%016lX",
                  i + 1, ip_probe_info[i]);
              status = false;
              break;
            }
          }
        }
        if (status) {
          JSON_POST_MSG(
              2,
              "Calculate number of probe (%d) must match paramter NO_PROBES=%d",
              native_probe, no_probes);
          if (native_probe != no_probes) {
            JSON_POST_MSG(3, "Error: Comparison failed");
            status = false;
          }
        }
        if (status) {
          JSON_POST_MSG(2,
                        "OCLA Core Module must be associated with at least "
                        "with one probe (except AXI probe)");
          for (auto& m : ocla_modules) {
            if (!m->is_axi && m->probe_order.size() == 0) {
              JSON_POST_MSG(3,
                            "Error: NATIVE OCLA module at INDEX=%d does not "
                            "have any probe",
                            m->index);
              status = false;
            } else if (m->is_axi && m->probe_order.size() != 0) {
              JSON_POST_MSG(
                  3, "Error: Detect probe at AXI OCLA module at INDEX=%d",
                  m->index);
              status = false;
            }
          }
        }
      } else {
        JSON_POST_MSG(3, "Error: Estimated Native Cores value %d is invalid",
                      native_core);
        status = false;
      }
      if (status) {
        JSON_POST_MSG(1, "Core{x}_Width information:")
        for (uint32_t i = 0; i < native_core; i++) {
          JSON_POST_MSG(2, "Calculated Core%02d_Width=%d", i + 1,
                        calculated_ip_core_width[i]);
        }
      }
    }
    return status;
  }
  std::string mode = "";
  std::string axi_type = "";
  std::string sampling_clk = "";
  uint32_t cores = 0;
  uint32_t no_probes = 0;
  uint32_t no_axi_bus = 0;
  uint32_t probes_sum = 0;
  uint32_t ip_probe_width[MAXIMUM_SUPPORTED_PROBE_CORE] = {0};
  uint32_t ip_address[MAXIMUM_SUPPORTED_PROBE_CORE] = {0};
  uint64_t ip_probe_info[MAXIMUM_SUPPORTED_PROBE_CORE] = {0};
  uint32_t axi_core_address = 0;
  PROBE2CORE_PARAM_INFO probe_to_core_map[MAXIMUM_SUPPORTED_PROBE_CORE];
  uint32_t calculated_ip_core_width[MAXIMUM_SUPPORTED_PROBE_CORE] = {0};
};

class OCLA_Analyzer {
 public:
  /*
    The only public access static function
    This is entry to analyze the design
      a. Input is from RTLIL::Design
      b. Output is dumped into json std::ofstream
  */
  static void analyze(RTLIL::Design* design, std::ofstream& json) {
    printf("************************************\n");
    printf("************************************\n");
    json << "{\n  \"messages\" : [\n";
    JSON_POST_MSG(0, "Start of OCLA Analysis");
    if (design->top_module() == nullptr) {
      JSON_POST_MSG(0, "Cannot find top module");
      json << "    \"End of OCLA Analysis\"\n  ]";
      json << "\n}\n";
      json.close();
      log_error("Cannot find top module\n");
    }
    uint32_t ocla_count = 0;
    std::string cmd = "";
    std::string ocla_debug_subsystem_instantiator = "";
    std::string ocla_debug_subsystem_connection_name = "";
    std::vector<OCLA_MODULE*> ocla_modules;
    std::vector<OCLA_DEBUG_SUBSYSTEM_MODULE*> ocla_debug_subsystem_modules;
    std::vector<std::string> ocla_instantiator_names;
    // Step 1: Get all the OCLA and OCLA Debug Subsystem IPs
    get_modules(design, ocla_modules, ocla_debug_subsystem_modules, json);

    // Step 2: Check the detected IP counts
    //    a. User can instantiate as many OCLA instances
    //    b. They are all instantiated by OCLA Debug Subsystem
    if (ocla_modules.size() == 0 || ocla_debug_subsystem_modules.size() != 1) {
      JSON_POST_MSG(0,
                    "Warning/Error: OCLA module count=%ld, OCLA Debug "
                    "Subsystem module count=%ld",
                    ocla_modules.size(), ocla_debug_subsystem_modules.size());
      goto ANALYZE_MSG_END;
    }

    // Step 3: Make sure there is only one OCLA Debug Subsystem all the way up
    // to top
    if (!check_unique_ocla_debug_subsystem(
            design, ocla_debug_subsystem_modules[0]->name,
            ocla_debug_subsystem_instantiator,
            ocla_debug_subsystem_connection_name, json)) {
      JSON_POST_MSG(1,
                    "Error: Currently only support one OCLA Debug Subsystem "
                    "instance in a design");
      goto ANALYZE_MSG_END;
    }

    // Step 4: For each OCLA IP, grab all the instantiator (or wrapper)
    for (auto& o : ocla_modules) {
      get_ocla_instantiator(design, o, ocla_instantiator_names, json);
    }

    // Step 5: Make sure we successfully grab at least 1 instantiator
    if (ocla_instantiator_names.size() == 0) {
      JSON_POST_MSG(0, "Error: Does not find any OCLA instantiator");
      goto ANALYZE_MSG_END;
    }

    // Step 6: Set the last OCLA IP as axi
    if (ocla_debug_subsystem_modules[0]->mode == "AXI" ||
        ocla_debug_subsystem_modules[0]->mode == "NATIVE_AXI") {
      ocla_modules.back()->is_axi = true;
    }

    // Step 7: Match OCLA instantiator
    if (!sanity_check(ocla_debug_subsystem_modules[0], ocla_modules,
                      ocla_instantiator_names, json)) {
      JSON_POST_MSG(0, "Error: Sanity check fail");
      goto ANALYZE_MSG_END;
    }

    // Step 8: Black box OCLA Debug Subsystem instantiator module and Flatten
    // the design
#if 1
    cmd = gen_string("blackbox %s", ocla_debug_subsystem_instantiator.c_str());
    JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
    run_pass(cmd.c_str(), design);
#elif 0
    cmd = gen_string("blackbox %s",
                     ocla_debug_subsystem_modules[0]->name.c_str());
    JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
    run_pass(cmd.c_str(), design);
#else
    for (auto& o : ocla_modules) {
      cmd = gen_string("blackbox %s", o->name.c_str());
      JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
      run_pass(cmd.c_str(), design);
    }
#endif
    cmd = "flatten";
    JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
    run_pass(cmd.c_str(), design);
#if 0
    cmd = "write_rtlil ocla.rtlil";
    JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
    run_pass(cmd.c_str(), design);
#endif

    // Step 9: Once the flatten the design, start to grab all the signals
    // information
    if (!get_ocla_signals(design->top_module(),
                          ocla_debug_subsystem_modules[0]->mode == "NATIVE"
                              ? "NATIVE"
                              : ocla_debug_subsystem_modules[0]->axi_type,
                          ocla_debug_subsystem_modules[0]->no_axi_bus,
                          ocla_modules, ocla_debug_subsystem_instantiator,
                          json)) {
      JSON_POST_MSG(0, "Error: Fail to get probe signals");
      goto ANALYZE_MSG_END;
    }

    // Step 10: Loop through the instantiator that we gathered so far and
    // perform final validation
    for (auto& o : ocla_modules) {
      JSON_POST_MSG(1, "Module: %s", o->name.c_str());
      JSON_POST_MSG(2, "Final checking ...");
      if (o->finalize(
              json, &(ocla_debug_subsystem_modules[0]->ip_probe_width[0]), 3)) {
        JSON_POST_MSG(3, "Probes:");
        for (auto& sig : o->probes) {
          JSON_POST_MSG(4, "--> %s", sig.fullname.c_str());
          JSON_POST_MSG(5, ": %s (width=%d, offset=%d)", sig.name.c_str(),
                        sig.width, sig.offset);
        }
        ocla_count++;
      } else {
        JSON_POST_MSG(3, "Error: Disqualify this module");
        ocla_count = 0;
        break;
      }
    }
  ANALYZE_MSG_END:
    json << "    \"End of OCLA Analysis\"\n  ]";

    // Step 11: There is no error detected in all OCLA instance, then we dump
    // those information is JSON file
    if (ocla_count) {
      json << ",\n  \"ocla\" : [\n";
      uint32_t index = 0;
      for (auto& o : ocla_modules) {
        json << "    {\n", json_write_param(o, json, 3);
        json << gen_string(",\n      \"addr\" : %d", o->base_address).c_str();
        json << gen_string(",\n      \"probe_info\" : [\n").c_str();
        size_t order_index = 0;
        for (auto& p : o->probe_order) {
          json << "        {\n";
          json << gen_string("          \"index\" : %d,\n", p).c_str();
          json << gen_string("          \"offset\" : %d,\n",
                             ocla_debug_subsystem_modules[0]
                                 ->probe_to_core_map[p]
                                 .offset)
                      .c_str();
          json << gen_string("          \"width\" : %d\n",
                             ocla_debug_subsystem_modules[0]->ip_probe_width[p])
                      .c_str();
          json << "        }";
          order_index++;
          if (order_index < o->probe_order.size()) {
            json << ",\n";
          } else {
            json << "\n";
          }
        }
        json << gen_string("      ]").c_str();
        json_write_signals("probes", o->probes, json);
        index++;
        if (index < ocla_count) {
          json << "    },\n";
        } else {
          json << "    }\n";
        }
      }
      json << "  ]";
      json << ",\n  \"ocla_debug_subsystem\" : {\n";
      json_write_param(ocla_debug_subsystem_modules[0], json, 2);
      json << "\n  }";
    }

    // Step 12: Take care of memory leak
    while (ocla_modules.size()) {
      delete ocla_modules.back();
      ocla_modules.pop_back();
    }
    while (ocla_debug_subsystem_modules.size()) {
      delete ocla_debug_subsystem_modules.back();
      ocla_debug_subsystem_modules.pop_back();
    }
    json << "\n}\n";
  }

 private:
  /*
    Convert RTLIL::Const to string: normally is parameter or const signal
    (example: 4'b0000, 5'h3)
  */
  static void dump_const(std::ostringstream& f, const RTLIL::Const& data,
                         int width = -1, int offset = 0, bool autoint = true) {
    if (width < 0) {
      width = data.bits.size() - offset;
    }
    if ((data.flags & RTLIL::CONST_FLAG_STRING) == 0 ||
        width != (int)data.bits.size()) {
      if (width == 32 && autoint) {
        int32_t val = 0;
        for (int i = 0; i < width; i++) {
          log_assert(offset + i < (int)data.bits.size());
          switch (data.bits[offset + i]) {
            case State::S0:
              break;
            case State::S1:
              val |= 1 << i;
              break;
            default:
              val = -1;
              break;
          }
        }
        if (val >= 0) {
          f << stringf("%d", val);
          return;
        }
      }
      f << stringf("%d'", width);
      if (data.is_fully_undef()) {
        f << "x";
      } else {
        for (int i = offset + width - 1; i >= offset; i--) {
          log_assert(i < (int)data.bits.size());
          switch (data.bits[i]) {
            case State::S0:
              f << stringf("0");
              break;
            case State::S1:
              f << stringf("1");
              break;
            case RTLIL::Sx:
              f << stringf("x");
              break;
            case RTLIL::Sz:
              f << stringf("z");
              break;
            case RTLIL::Sa:
              f << stringf("-");
              break;
            case RTLIL::Sm:
              f << stringf("m");
              break;
          }
        }
      }
    } else {
      f << stringf("\"");
      std::string str = data.decode_string();
      for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '\n')
          f << stringf("\\n");
        else if (str[i] == '\t')
          f << stringf("\\t");
        else if (str[i] < 32)
          f << stringf("\\%03o", str[i]);
        else if (str[i] == '"')
          f << stringf("\\\"");
        else if (str[i] == '\\')
          f << stringf("\\\\");
        else
          f << str[i];
      }
      f << stringf("\"");
    }
  }
  /*
    Convert RTLIL::SigSpec to string/OCLA_SIGNAL
  */
  static void dump_sigspec(std::ostringstream& f, std::vector<OCLA_SIGNAL>& ss,
                           const RTLIL::SigSpec& sig, bool autoint = true) {
    if (sig.is_chunk()) {
      OCLA_SIGNAL s = dump_sigchunk(f, sig.as_chunk(), autoint);
      ss.insert(ss.begin(), s);
    } else {
      f << stringf("{ ");
      for (auto it = sig.chunks().rbegin(); it != sig.chunks().rend(); ++it) {
        OCLA_SIGNAL s = dump_sigchunk(f, *it, false);
        ss.insert(ss.begin(), s);
        f << stringf(" ");
      }
      f << stringf("}");
    }
  }
  /*
    Convert RTLIL::SigChunk to string/OCLA_SIGNAL
  */
  static OCLA_SIGNAL dump_sigchunk(std::ostringstream& f,
                                   const RTLIL::SigChunk& chunk, bool autoint) {
    std::ostringstream temp;
    std::string name = "";
    bool show_index = false;
    if (chunk.wire == NULL) {
      dump_const(temp, chunk.data, chunk.width, chunk.offset, autoint);
      name = temp.str();
    } else {
      name = chunk.wire->name.c_str();
      show_index = !(chunk.width == chunk.wire->width && chunk.width == 1 &&
                     chunk.offset == 0);
      if (chunk.width == chunk.wire->width && chunk.offset == 0) {
        temp << stringf("%s", chunk.wire->name.c_str());
      } else if (chunk.width == 1) {
        temp << stringf("%s [%d]", chunk.wire->name.c_str(), chunk.offset);
      } else {
        temp << stringf("%s [%d:%d]", chunk.wire->name.c_str(),
                        chunk.offset + chunk.width - 1, chunk.offset);
      }
    }
    f << temp.str().c_str();
    return OCLA_SIGNAL(temp.str(), name, (uint32_t)(chunk.width),
                       (uint32_t)(chunk.offset), show_index);
  }
  /*
    Check if the module match the module name that we are looking for
  */
  static bool match_module_name(const RTLIL::Module* module,
                                const std::string& module_name) {
    log_assert(module != nullptr);
    log_assert(module_name.size());
    std::string fullname = gen_string("\\%s", module_name.c_str());
    return module->name == fullname ||
           (module->name.size() > fullname.size() &&
            module->name.substr(module->name.size() - fullname.size()) ==
                fullname);
  }
  /*
    Get OCLA and OCLA Debug Subsystem module (if there is any)
  */
  static void get_modules(
      RTLIL::Design* design, std::vector<OCLA_MODULE*>& ocla_modules,
      std::vector<OCLA_DEBUG_SUBSYSTEM_MODULE*>& ocla_debug_subsystem_modules,
      std::ofstream& json) {
    for (auto module : design->modules()) {
      if (match_module_name(module, "ocla")) {
        printf("OCLA Module: %s\n", module->name.c_str());
        JSON_POST_MSG(0, "Detected Potential OCLA: %s", module->name.c_str());
        OCLA_MODULE* m = new (OCLA_MODULE)(module->name.c_str());
        log_assert(m != nullptr);
        MODULE_IP* ip = (MODULE_IP*)(m);
        get_module_params(module, ip, json);
        if (ip != nullptr && m->check_type(json)) {
          auto iter = ocla_modules.begin();
          while (iter != ocla_modules.end()) {
            if (m->index < (*iter)->index) {
              break;
            }
            iter++;
          }
          if (iter != ocla_modules.end()) {
            ocla_modules.insert(iter, m);
          } else {
            ocla_modules.push_back(m);
          }
          JSON_POST_MSG(1, "Qualified as OCLA module");
        } else {
          JSON_POST_MSG(1, "Error: this is not qualified as OCLA module");
        }
      } else if (match_module_name(module, "ocla_debug_subsystem")) {
        printf("OCLA Debug Subsystem Module: %s\n", module->name.c_str());
        JSON_POST_MSG(0, "Detected Potential OCLA Debug Subsystem: %s",
                      module->name.c_str());
        OCLA_DEBUG_SUBSYSTEM_MODULE* m =
            new (OCLA_DEBUG_SUBSYSTEM_MODULE)(module->name.c_str());
        log_assert(m != nullptr);
        MODULE_IP* ip = (MODULE_IP*)(m);
        get_module_params(module, ip, json);
        if (ip != nullptr && m->check_type(json)) {
          ocla_debug_subsystem_modules.push_back(m);
          JSON_POST_MSG(1, "Qualified as OCLA Debug Subsystem module");
        } else {
          JSON_POST_MSG(
              1, "Error: this is not qualified as OCLA Debug Subsystem module");
        }
      }
    }
  }
  /*
    Make sure there is only one OCLA Debug Subsystem IP being instantiated - all
    the way up to top
  */
  static bool check_unique_ocla_debug_subsystem(RTLIL::Design* design,
                                                std::string module_name,
                                                std::string& instantiator,
                                                std::string& connection_name,
                                                std::ofstream& json) {
    bool status = true;
    RTLIL::Module* top_module = design->top_module();
    JSON_POST_MSG(0, "Check uniqueness of OCLA Debug Subsystem");
    int level = 0;
    instantiator = "";
    connection_name = "";
    while (status) {
      JSON_POST_MSG(1, "Module: %s", module_name.c_str());
      std::vector<std::string> module_names;
      for (auto m : design->modules()) {
        for (auto cell : m->cells()) {
          if (std::string(cell->type.c_str()) == module_name) {
            JSON_POST_MSG(2, "Instantiated by %s as %s", m->name.c_str(),
                          cell->name.c_str());
            module_names.push_back(m->name.c_str());
            if (level) {
              if (connection_name.size()) {
                connection_name = gen_string("%s.%s", cell->name.c_str(),
                                             connection_name.substr(1).c_str());
              } else {
                connection_name = cell->name.c_str();
              }
            }
          }
        }
      }
      level++;
      if (module_names.size() == 1) {
        module_name = module_names[0];
        if (std::string(top_module->name.c_str()) == module_name) {
          JSON_POST_MSG(3, "This is top module");
          if (level >= 2) {
            JSON_POST_MSG(3, "Connection chain for OCLA Debug Subsystem: %s",
                          connection_name.c_str());
          } else {
            JSON_POST_MSG(3,
                          "Hierarchy level for OCLA Debug Subsystem is out of "
                          "expectation");
            status = false;
          }
          break;
        }
        if (level == 1) {
          instantiator = module_names[0];
        }
      } else {
        status = false;
      }
    }
    if (status && instantiator.size() > 0) {
      JSON_POST_MSG(1, "OCLA Debug Subsystem Instantiator: %s",
                    instantiator.c_str());
    }
    return status;
  }
  /*
    Sanity check of all retrieved parameter information
  */
  static bool sanity_check(
      const OCLA_DEBUG_SUBSYSTEM_MODULE* ocla_debug_subsystem_module,
      const std::vector<OCLA_MODULE*> ocla_modules,
      const std::vector<std::string> ocla_instantiator_names,
      std::ofstream& json) {
    JSON_POST_MSG(0, "Sanity Check");
    log_assert(ocla_modules.size());
    bool status = true;
    if (ocla_modules.size() != ocla_instantiator_names.size()) {
      JSON_POST_MSG(1,
                    "Error: Not all the OCLA module (count=%ld) found the "
                    "instantiator (count=%ld)",
                    ocla_modules.size(), ocla_instantiator_names.size());
      status = false;
    }
    if (status) {
      if (ocla_debug_subsystem_module->cores !=
          (uint32_t)(ocla_modules.size())) {
        JSON_POST_MSG(1,
                      "Error: OCLA Debug Subsystem paramter CORES=%d does not "
                      "match with detected OCLA module count=%ld",
                      ocla_debug_subsystem_module->cores, ocla_modules.size());
        status = false;
      }
    }
    if (status) {
      JSON_POST_MSG(1, "Check module parameter INDEX sequence, must be 0 .. %d",
                    ocla_debug_subsystem_module->cores - 1);
      uint32_t sequence = 0;
      for (auto m : ocla_modules) {
        if (m->index != sequence) {
          JSON_POST_MSG(2,
                        "Error: Module %s has unexpected INDEX, "
                        "expectation=%d, but found %d",
                        m->name.c_str(), sequence, m->index);
          status = false;
        }
        sequence++;
      }
    }
    if (status) {
      std::string expected_instantiator_name =
          ocla_debug_subsystem_module->name;
      JSON_POST_MSG(1, "All modules should be instantiated by %s",
                    expected_instantiator_name.c_str());
      for (auto n : ocla_instantiator_names) {
        if (n != expected_instantiator_name) {
          JSON_POST_MSG(2, "Found unexpected instantiator: %s", n.c_str());
          status = false;
        }
      }
    }
    if (status) {
      JSON_POST_MSG(
          1, "Parameter IP_TYPE=%s, IP_VERSION=0x%08X, IP_ID=0x%08X must match",
          ocla_debug_subsystem_module->type.c_str(),
          ocla_debug_subsystem_module->version,
          ocla_debug_subsystem_module->id);
      for (auto m : ocla_modules) {
        if (ocla_debug_subsystem_module->type != m->type ||
            ocla_debug_subsystem_module->version != m->version ||
            ocla_debug_subsystem_module->id != m->id) {
          JSON_POST_MSG(2,
                        "Error: Module %s has mismatch paramerer IP_TYPE=%s, "
                        "IP_VERSION=0x%08X, IP_ID=0x%08X",
                        m->name.c_str(), m->type.c_str(), m->version, m->id);
          status = false;
        }
      }
    }
    if (status) {
      uint32_t axi_addr_width = ocla_modules[0]->axi_addr_width;
      uint32_t axi_data_width = ocla_modules[0]->axi_data_width;
      JSON_POST_MSG(1,
                    "Parameter AXI_ADDR_WIDTH=%d, AXI_DATA_WIDTH=%d must match",
                    axi_addr_width, axi_data_width);
      for (auto m : ocla_modules) {
        if (axi_addr_width != m->axi_addr_width ||
            axi_data_width != m->axi_data_width) {
          JSON_POST_MSG(2,
                        "Error: Module %s has mismatch paramerer "
                        "AXI_ADDR_WIDTH=%d, AXI_DATA_WIDTH=%d",
                        m->name.c_str(), m->axi_addr_width, m->axi_data_width);
          status = false;
        }
      }
    }
    if (status) {
      JSON_POST_MSG(1, "Probe <-> Core information mapping must be valid");
      status = (const_cast<OCLA_DEBUG_SUBSYSTEM_MODULE*>(
                    ocla_debug_subsystem_module))
                   ->map_probe_core(json, ocla_modules);
    }
    if (status) {
      JSON_POST_MSG(1, "Parameter NO_OF_PROBES and Param{x}_Width must match");
      for (auto m : ocla_modules) {
        if (m->is_axi) {
          JSON_POST_MSG(1,
                        "Last OCLA INDEX=%d must match AXI Protocol Bus Size",
                        m->index);
          if (ocla_debug_subsystem_module->calculated_ip_core_width[m->index] !=
              0) {
            JSON_POST_MSG(
                2,
                "Error: Instantiator calculated Core%02d_Width=%d, but expect "
                "it "
                "is 0 (last OCLA is connected to AXI)",
                m->index + 1,
                ocla_debug_subsystem_module
                    ->calculated_ip_core_width[m->index]);
            status = false;
          }
          uint32_t axi_expected_probes_count = 0;
          uint32_t protocol_size = 0;
          if (ocla_debug_subsystem_module->axi_type == "AXILite") {
            protocol_size = AXILite_SINGLE_BUS_SIGNALS;
          } else {
            protocol_size = AXI4_SINGLE_BUS_SIGNALS;
          }
          axi_expected_probes_count =
              ocla_debug_subsystem_module->no_axi_bus * protocol_size;
          if (m->probes_count != axi_expected_probes_count) {
            JSON_POST_MSG(
                2,
                "Error: Module %s has mismatch paramerer "
                "NO_OF_PROBES=%d, but expected it is %d (count=%d x "
                "protocol_size=%d)",
                m->name.c_str(), m->probes_count, axi_expected_probes_count,
                ocla_debug_subsystem_module->no_axi_bus, protocol_size);
            status = false;
          }
        } else {
          if (m->probes_count !=
              ocla_debug_subsystem_module->calculated_ip_core_width[m->index]) {
            JSON_POST_MSG(
                2,
                "Error: Module %s has mismatch paramerer "
                "NO_OF_PROBES=%d, instantiator calculated Core%02d_Width=%d",
                m->name.c_str(), m->probes_count, m->index + 1,
                ocla_debug_subsystem_module
                    ->calculated_ip_core_width[m->index]);
            status = false;
          }
        }
      }
    }
    if (status && ocla_modules.size() < MAXIMUM_SUPPORTED_PROBE_CORE) {
      JSON_POST_MSG(1, "Unused Probe[%02ld..15]_Width must be null",
                    ocla_modules.size() + 1);
      for (uint32_t i = ocla_debug_subsystem_module->cores;
           i < MAXIMUM_SUPPORTED_PROBE_CORE; i++) {
        if (ocla_debug_subsystem_module->calculated_ip_core_width[i]) {
          JSON_POST_MSG(2, "Error: Probe%d is not null", i);
          status = false;
        }
      }
    }
    if (status) {
      JSON_POST_MSG(1, "Parameter PROBES_SUM versus Probe{x}_Width");
      uint32_t probes_sum = 0;
      for (uint32_t i = 0; i < MAXIMUM_SUPPORTED_PROBE_CORE; i++) {
        probes_sum += ocla_debug_subsystem_module->ip_probe_width[i];
      }
      if (ocla_debug_subsystem_module->mode != "NATIVE") {
        // Last one will be AXI
        probes_sum += ocla_modules.back()->probes_count;
      }
      if (probes_sum != ocla_debug_subsystem_module->probes_sum) {
        JSON_POST_MSG(2,
                      "Error: PROBES_SUM by calculation (%d) does not match "
                      "with definition (%d)",
                      probes_sum, ocla_debug_subsystem_module->probes_sum);
        status = false;
      }
    }
    if (status) {
      JSON_POST_MSG(1, "Parameter PROBES_SUM versus calculated Core{x}_Width");
      uint32_t probes_sum = 0;
      for (uint32_t i = 0; i < MAXIMUM_SUPPORTED_PROBE_CORE; i++) {
        probes_sum += ocla_debug_subsystem_module->calculated_ip_core_width[i];
      }
      if (ocla_debug_subsystem_module->mode != "NATIVE") {
        // Last one will be AXI
        probes_sum += ocla_modules.back()->probes_count;
      }
      if (probes_sum != ocla_debug_subsystem_module->probes_sum) {
        JSON_POST_MSG(2,
                      "Error: PROBES_SUM by calculation (%d) does not match "
                      "with definition (%d)",
                      probes_sum, ocla_debug_subsystem_module->probes_sum);
        status = false;
      }
    }
    if (status) {
      JSON_POST_MSG(1, "Parameter IF[01..%02ld]_BaseAddress must not conflict",
                    ocla_modules.size());
      std::vector<uint32_t> addresses;
      for (auto m : ocla_modules) {
        m->base_address = ocla_debug_subsystem_module->ip_address[m->index];
        if (std::find(addresses.begin(), addresses.end(), m->base_address) ==
            addresses.end()) {
          JSON_POST_MSG(2, "Module %s has base address 0x%08X", m->name.c_str(),
                        m->base_address);
          addresses.push_back(m->base_address);
        } else {
          JSON_POST_MSG(2, "Error: Module %s has conflict base address 0x%08X",
                        m->name.c_str(), m->base_address);
          status = false;
        }
      }
    }
    return status;
  }
  /*
    Retrieve parameter from the module
  */
  static void get_module_params(RTLIL::Module* module, MODULE_IP*& ip,
                                std::ofstream& json) {
    for (const auto& p : module->avail_parameters) {
      const auto& it = module->parameter_default_values.find(p);
      if (it != module->parameter_default_values.end()) {
        std::ostringstream param;
        dump_const(param, it->second);
        if (!ip->set_param(json, p.c_str(), param.str().c_str())) {
          delete ip;
          ip = nullptr;
          break;
        }
      }
    }
    if (ip != nullptr) {
      if (!ip->check_all_params(json)) {
        delete ip;
        ip = nullptr;
      }
    }
  }
  /*
    Get the information of OCLA instantiator/wrapper
      a. This function only get the information name.
      b. This is done before we blackbox the instantiator and flatten the design
  */
  static void get_ocla_instantiator(RTLIL::Design* design, OCLA_MODULE* module,
                                    std::vector<std::string>& instantiators,
                                    std::ofstream& json) {
    bool found = false;
    JSON_POST_MSG(0, "Check instantiator for OCLA module %s",
                  module->name.c_str());
    for (auto m : design->modules()) {
      for (auto cell : m->cells()) {
        if (std::string(cell->type.c_str()) == module->name) {
          JSON_POST_MSG(1, "Instantiated by %s", m->name.c_str());
          instantiators.push_back(std::string(m->name.c_str()));
          found = true;
        }
      }
    }
    if (!found) {
      JSON_POST_MSG(1, "Warning: Does not detect any instantiator");
    }
  }
  /*
    Get the information of OCLA instantiator/wrapper
      a. This function retrieve all other information that we need:
          - probed signals
          - trigger signals
      b. This is done after we blackbox the instantiator and flatten the design
  */
  static bool get_ocla_signals(RTLIL::Module* top_module,
                               const std::string& axi_type, uint32_t no_axi_bus,
                               std::vector<OCLA_MODULE*>& modules,
                               std::string instantiator_module,
                               std::ofstream& json) {
    bool status = true;
    JSON_POST_MSG(0, "Retrieve OCLA signals (type=%s) from instantiator: %s",
                  axi_type.c_str(), instantiator_module.c_str());
    log_assert(modules.size());
    for (auto m : modules) {
      log_assert(m->probes.size() == 0);
    }
    for (auto cell : top_module->cells()) {
      if (std::string(cell->type.c_str()) == instantiator_module) {
        JSON_POST_MSG(1, "Instantiated as %s", cell->name.c_str());
        for (auto m : modules) {
          if (m->is_axi) {
            continue;
          }
          for (int i = (int)(m->probe_order.size() - 1); i >= 0; i--) {
            std::string module_probe_name =
                gen_string("\\probe_%d", m->probe_order.at(i) + 1);
            JSON_POST_MSG(2,
                          "OCLA Module at INDEX=%d looking for connection %s",
                          m->index, module_probe_name.c_str());
            bool found = false;
            for (auto& connection : cell->connections()) {
              std::ostringstream wire;
              std::string connection_name =
                  std::string(connection.first.c_str());
              if (connection_name == module_probe_name) {
                found = true;
                JSON_POST_MSG(3, "Found potential Probe Connection: %s",
                              connection_name.c_str());
                size_t starting_count = m->probes.size();
                dump_sigspec(wire, m->probes, connection.second);
                JSON_POST_MSG(4, "Connected to %s", wire.str().c_str());
                if (m->probes.size() <= starting_count) {
                  JSON_POST_MSG(4, "Fail to parse connection %s",
                                module_probe_name.c_str());
                  status = false;
                }
              }
            }
            if (!found) {
              JSON_POST_MSG(3, "Fail to find the connection");
              status = false;
            }
          }
        }
      }
    }
    if (status) {
      for (auto m : modules) {
        if (m->is_axi) {
          if (m->probes.size() != 0) {
            JSON_POST_MSG(2,
                          "Module %s (INDEX=%d) is AXI protocol, there "
                          "shouldn't be any probe signal, but found there is",
                          m->name.c_str(), m->index);
            status = false;
          }
          if (status) {
            if (axi_type == "AXILite") {
              for (uint32_t i = 0; i < no_axi_bus; i++) {
                m->probes.push_back(
                    OCLA_SIGNAL("AWADDR", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWPROT", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WDATA", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WSTRB", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BRESP", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARADDR", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARPROT", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RDATA", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RRESP", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RREADY", 1, i, no_axi_bus == 1));
              }
            } else {
              for (uint32_t i = 0; i < no_axi_bus; i++) {
                m->probes.push_back(
                    OCLA_SIGNAL("AWADDR", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWPROT", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWBURST", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWSIZE", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWLEN", 8, i, no_axi_bus == 1));
                m->probes.push_back(OCLA_SIGNAL("AWID", 8, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWCACHE", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWREGION", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWUSER", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWQOS", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("AWLOCK", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WDATA", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WSTRB", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(OCLA_SIGNAL("WID", 8, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("WLAST", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BRESP", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(OCLA_SIGNAL("BID", 8, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("BUSER", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARADDR", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARPROT", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARBUSRT", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARSIZE", 3, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARLEN", 8, i, no_axi_bus == 1));
                m->probes.push_back(OCLA_SIGNAL("ARID", 8, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARCACHE", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARREGION", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARUSER", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARQOS", 4, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("ARLOCK", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RDATA", 32, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RRESP", 2, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RREADY", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RVALID", 1, i, no_axi_bus == 1));
                m->probes.push_back(OCLA_SIGNAL("RID", 8, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RUSER", 1, i, no_axi_bus == 1));
                m->probes.push_back(
                    OCLA_SIGNAL("RLAST", 1, i, no_axi_bus == 1));
              }
            }
          }
        } else {
          if (m->probes.size() == 0) {
            JSON_POST_MSG(2, "Module %s (INDEX=%d) failed to get probe signals",
                          m->name.c_str(), m->index);
            status = false;
          }
        }
      }
    }
    return status;
  }
  /*
    Write out IP/Module parameter into JSON file
  */
  static void json_write_param(MODULE_IP* ip, std::ofstream& json,
                               uint32_t space) {
    std::string info = "";
    size_t index = 0;
    for (auto& p : ip->params) {
      for (uint32_t i = 0; i < space; i++) {
        json << "  ";
      }
      if (p.second.type == PARAM_IS_STR) {
        std::string* ptr = (std::string*)(p.second.ptr);
        info = gen_string("\"%s\" : \"%s\"", p.first.c_str(), ptr->c_str());
      } else if (p.second.type == PARAM_IS_UINT64) {
        uint64_t* ptr = (uint64_t*)(p.second.ptr);
        info = gen_string("\"%s\" : %lu", p.first.c_str(), *ptr);
      } else {
        uint32_t* ptr = (uint32_t*)(p.second.ptr);
        info = gen_string("\"%s\" : %u", p.first.c_str(), *ptr);
      }
      for (auto& c : info) {
        if (c != '\\') {
          json << c;
        }
      }
      index++;
      if (index < ip->params.size()) {
        json << ",\n";
      }
    }
  }
  /*
    Write out IP/Module signals into JSON file
  */
  static void json_write_signals(std::string name,
                                 std::vector<OCLA_SIGNAL>& signals,
                                 std::ofstream& json) {
    json << gen_string(",\n      \"%s\" : [\n", name.c_str()).c_str();
    size_t index = 0;
    for (auto& s : signals) {
#if 0
      std::string name = s.fullname;
      size_t sindex = name.rfind(".");
      if (sindex != std::string::npos) {
        name = name.substr(sindex+1);
      }
      if (name.size() > 0 && name[0] == '\\') {
        name = name.substr(1);
      }
      json << gen_string("        \"%s\"", name.c_str()).c_str();
#else
      if (!s.show_index) {
        json << gen_string("        \"%s\"", s.name.c_str()).c_str();
      } else if (s.width == 1) {
        json << gen_string("        \"%s[%d]\"", s.name.c_str(), s.offset)
                    .c_str();
      } else {
        json << gen_string("        \"%s[%d:%d]\"", s.name.c_str(),
                           s.offset + s.width - 1, s.offset)
                    .c_str();
      }
#endif
      index++;
      if (index == signals.size()) {
        json << "\n";
      } else {
        json << ",\n";
      }
    }
    json << "      ]\n";
  }
};

struct OCLA_AnalyzerPass : public Pass {
  OCLA_AnalyzerPass()
      : Pass("ocla_analyze",
             "Analyze OCLA information from the design for Raptor") {}

  void help() override {
    log("\n");
    log("    ocla_analyze\n");
    log("\n");
    log("Analyze OCLA information from the design for Raptor and write out "
        "'ocla.json'\n");
    log("\n");
    log("    -top <top_module_name>\n");
    log("       performs Analyze from the top module with name "
        "'top_module_name'.\n");
    log("    -auto-top\n");
    log("       detects automatically the top module. If several tops, it "
        "picks up the one with deepest hierarchy. Analyze from this selected "
        "top module.\n");
    log("    -file <output json file>\n");
    log("       writes the output to the specified file. Optional, if not "
        "specified, the default name is ocla.json\n");
    log("\n");
  }

  void execute(std::vector<std::string> args, RTLIL::Design* design) override {
    // Parse Analyze command arguments
    std::string top_name = "";
    std::string json_name = "ocla.json";
    bool is_auto = false;
    size_t argidx;
    for (argidx = 1; argidx < args.size(); argidx++) {
      if (args[argidx] == "-top" && argidx + 1 < args.size()) {
        top_name = args[++argidx];
        continue;
      }
      if (args[argidx] == "-auto-top") {
        is_auto = true;
        continue;
      }
      if (args[argidx] == "-file" && argidx + 1 < args.size()) {
        json_name = args[++argidx];
        continue;
      }
      log_error("Analyze Unknown Option : \"%s\"\n", args[argidx].c_str());
    }
    extra_args(args, argidx, design);
    if (top_name.size()) {
      string cmd = "hierarchy -top " + top_name;
      run_pass(cmd.c_str());
    } else if (is_auto) {
      run_pass("hierarchy -auto-top");
    }
    std::ofstream json(json_name.c_str());
    OCLA_Analyzer::analyze(design, json);
    json.close();
  }
} OCLA_AnalyzerPass;

PRIVATE_NAMESPACE_END
