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
  This piece of code extract important information for RTLIL::Design class
  directly. These important information includes:
    a. Number of OCLA instances being instantiated (if there is)
    b. Each signals that user would like to probe/debug
    c. Memory Depth of the buffer to store raw data
    d. How each OCLA instance connected to AXIL interconnect
        -- useful for SW to determine what base addr to talk to each instance
    e. a lot more
*/

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/cellaigs.h"
#include "kernel/log.h"
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN
#define USE_LOCAL_GEN_STRING 0

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

struct PARAM_INFO {
  PARAM_INFO(void* p = nullptr, bool s = false) :
    is_str(s), ptr(p) {
  }
  bool is_str;
  bool is_assigned = false;
  void* ptr;
};

struct MODULE_IP {
  MODULE_IP(std::string n = "") : name (n) {
    log_assert(name.size());
    params["\\IP_TYPE"] = PARAM_INFO(&type, true);
    params["\\IP_VERSION"] = PARAM_INFO(&version);
    params["\\IP_ID"] = PARAM_INFO(&id);
  }
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
    if (params[name].is_str) {
      if (value.size() >= 2 && value[0] == '"' && value[value.size()-1] == '"') {
        std::string* string_ptr = (std::string*)(params[name].ptr);
        (* string_ptr) = value.substr(1, value.size() - 2);
        JSON_POST_MSG(1, "Param %s - %s", name.c_str(), string_ptr->c_str());
      } else {
        JSON_POST_MSG(1, "Error: Param %s value %s does not follow string format", name.c_str(), value.c_str());
        goto SET_PARAM_ERROR;
      }
    } else {
      uint32_t* number_ptr = (uint32_t*)(params[name].ptr);
      (* number_ptr) = (uint32_t)(atol(value.c_str()));
      JSON_POST_MSG(1, "Param %s - %d (0x%08X)", name.c_str(), (* number_ptr), (* number_ptr));
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

struct OCLA_MODULE : MODULE_IP {
  OCLA_MODULE(std::string n) : MODULE_IP(n) {
    params["\\AXI_ADDR_WIDTH"] = PARAM_INFO(&axi_addr_width);
    params["\\AXI_DATA_WIDTH"] = PARAM_INFO(&axi_data_width);
    params["\\MEM_DEPTH"] = PARAM_INFO(&mem_depth);
    params["\\PROBE_WIDHT"] = PARAM_INFO(&probe_width);
    params["\\NO_OF_PROBES"] = PARAM_INFO(&probes_count);
    params["\\NO_OF_TRIGGER_INPUTS"] = PARAM_INFO(&trigger_inputs_count);
  }
  bool check_type(std::ofstream& json) {
    bool status = false;
    if (type == "ocla" && mem_depth > 0 && probe_width > 0 && probe_width <= 32 &&
        probes_count > 0) {
      status = true;
    } else {
      JSON_POST_MSG(1, "Error: Fail to validate parameters");
      JSON_POST_MSG(2, "IP_TYPE: %s", type.c_str());
      JSON_POST_MSG(2, "MEM_DEPTH: %d", mem_depth);
      JSON_POST_MSG(2, "PROBE_WIDHT: %d", probe_width);
    }
    return status;
  }
  uint32_t axi_addr_width = 0;
  uint32_t axi_data_width = 0;
  uint32_t mem_depth = 0;
  uint32_t probe_width = 0;
  uint32_t probes_count = 0;
  uint32_t trigger_inputs_count = 0;
};

struct AXIL_INTERCONNECT_MODULE : MODULE_IP {
  AXIL_INTERCONNECT_MODULE(std::string n) : MODULE_IP(n) {
    params["\\ADDR_WIDTH"] = PARAM_INFO(&addr_width);
    params["\\DATA_WIDTH"] = PARAM_INFO(&data_width);
    params["\\M_COUNT"] = PARAM_INFO(&count);
    for (int i = 0; i < 16; i++) {
      params[gen_string("\\M%02d_BASE_ADDR", i)] = PARAM_INFO(&base_addresses[i]);
    }
  }
  bool check_type(std::ofstream& json) {
    bool status = false;
    if (type == "AXIL_IC" && count > 0 && count <= 16) {
      status = true;
    } else {
      JSON_POST_MSG(1, "Error: Fail to validate parameters");
      JSON_POST_MSG(2, "IP_TYPE: %s", type.c_str());
      JSON_POST_MSG(2, "M_COUNT: %d", count);
    }
    return status;
  }
  uint32_t addr_width = 0;
  uint32_t data_width = 0;
  uint32_t count = 0;
  uint32_t base_addresses[16];
  uint32_t tracking = 0;
};

struct OCLA_SIGNAL {
  OCLA_SIGNAL(std::string f, std::string n, uint32_t w, uint32_t o, bool s) :
    fullname(f), name(n), width(w), offset(o), show_index(s) {
    size_t index = name.rfind(".");
    if (index != std::string::npos) {
      name = name.substr(index+1);
    }
    if (name.size() > 0 && name[0] == '\\') {
      name = name.substr(1);
    }
    log_assert(width);
  }
  std::string fullname = "";
  std::string name = "";
  uint32_t width = 0;
  uint32_t offset = 0;
  bool show_index = false;
};

struct OCLA_INSTANTIATOR {
  OCLA_INSTANTIATOR(std::string m, std::string i, OCLA_MODULE* o,
                    std::vector<OCLA_SIGNAL> p, std::vector<OCLA_SIGNAL> t, 
                    OCLA_SIGNAL a, uint32_t in, std::vector<std::string> cs) :
    module(m), instance(i), ocla(o), probes(p), trigger_inputs(t), awready(a), 
    index(in), connection_chain(cs) {
  }
  bool finalize(AXIL_INTERCONNECT_MODULE* axil, std::ofstream& json, int space) {
    uint32_t total_s = 0;
    if (!status) {
      JSON_POST_MSG(space, "Error: Already has been marked as invalid instantiator");
      goto FINALIZE_END;
    }
    if (index >= axil->count) {
      JSON_POST_MSG(space, "Error: Invalid AXIL Interconnect connection index %d (AXIL master count %d)", 
                    index, axil->count);
      goto FINALIZE_ERROR;
    }

    if (axil->tracking & (1 << index)) {
      JSON_POST_MSG(space, "Error: Invalid AXIL Interconnect connection index %d because it had been used", 
                            index);
      goto FINALIZE_ERROR;
    } 
    
    // probes
    for (auto& s : probes) {
      total_s += s.width;
    }
    if (total_s != ocla->probes_count) {
      JSON_POST_MSG(space, "Error: Invalid total probe signal(s) bus size %d (NO_OF_PROBES %d)", 
                            total_s, ocla->probes_count);
      goto FINALIZE_ERROR;
    }
    // trigger inputs
    if (ocla->trigger_inputs_count > 0 && trigger_inputs.size() == 0) {
      ocla->trigger_inputs_count = 0;
      JSON_POST_MSG(space, "Warning: overwrite NO_OF_TRIGGER_INPUTS=0 since trigger_inputs signals is not specified");
    }
    total_s = 0;
    for (auto& s : trigger_inputs) {
      total_s += s.width;
    }
    if (total_s != ocla->trigger_inputs_count) {
      JSON_POST_MSG(space, "Error: Invalid total trigger_input signal(s) bus size %d (NO_OF_TRIGGER_INPUTS %d)", 
                            total_s, ocla->trigger_inputs_count);
      goto FINALIZE_ERROR;
    }
    // awready
    if (awready.width != 1) {
      JSON_POST_MSG(space, "Error: Invalid s_axil_awready bus size %d", awready.width);
      goto FINALIZE_ERROR;
    }
    addr = axil->base_addresses[index];
    axil->tracking |= (1 << index);
    goto FINALIZE_END;
    
FINALIZE_ERROR:
    status = false;
FINALIZE_END:
    return status;
  }
  std::string module = "";
  std::string instance = "";
  OCLA_MODULE* ocla = nullptr;
  std::vector<OCLA_SIGNAL> probes;
  std::vector<OCLA_SIGNAL> trigger_inputs;
  OCLA_SIGNAL awready;
  uint32_t index = 0;
  std::vector<std::string> connection_chain;
  uint32_t addr = 0;
  bool status = true;
};

class OCLA_Analyzer {
 public:
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
    std::string connection_name = "";
    std::vector<OCLA_MODULE*> ocla_modules;
    std::vector<AXIL_INTERCONNECT_MODULE*> axil_interconnect_modules;
    std::vector<std::string> ocla_instantiator_names;
    std::vector<OCLA_MODULE*> ocla_instantiated_modules;
    std::vector<OCLA_INSTANTIATOR> ocla_instantiators;
    get_modules(design, ocla_modules, axil_interconnect_modules, json);
    if (ocla_modules.size() == 0 || axil_interconnect_modules.size() != 1) {
      JSON_POST_MSG(0, "Warning/Error: OCLA module count=%ld, AXIL Interconnect module count=%ld", 
                        ocla_modules.size(), axil_interconnect_modules.size());
      goto ANALYZE_MSG_END;
    }
    // Make sure there is only one AXIL Interconnect all the way up to top
    if (!check_unique_axil_interconnect(design, axil_interconnect_modules[0]->name, connection_name, json)) {
      JSON_POST_MSG(1, "Error: Currently only support one AXIL Interconnect instance in a design");
      goto ANALYZE_MSG_END;
    }
    for (auto& o : ocla_modules) {
      get_ocla_instantiator(design, o, ocla_instantiator_names, ocla_instantiated_modules, json);
    }
    if (ocla_instantiator_names.size() == 0) {
      JSON_POST_MSG(0, "Error: Does not find any OCLA instantiator");
      goto ANALYZE_MSG_END;
    }
    log_assert(ocla_instantiator_names.size() == ocla_instantiated_modules.size());
    // Black box and Flatten the design
    for (auto& n : ocla_instantiator_names) {
      std::string cmd = gen_string("blackbox %s", n.c_str());
      JSON_POST_MSG(0, "Run command: %s", cmd.c_str());
      run_pass(cmd.c_str(), design);
    }
    JSON_POST_MSG(0, "Run command: flatten");
    run_pass("flatten", design);
    for (size_t i = 0; i < ocla_instantiator_names.size(); i++) {
      get_ocla_instantiator(design->top_module(), ocla_instantiator_names[i], ocla_instantiated_modules[i], 
                            ocla_instantiators, connection_name, json);
    }
    JSON_POST_MSG(0, "Total detected OCLA Instantiator: %ld", ocla_instantiators.size());
    for (auto& inst : ocla_instantiators) {
      JSON_POST_MSG(1, "Instance: %s, Module: %s", inst.instance.c_str(), inst.module.c_str());
      JSON_POST_MSG(2, "Final checking ...");
      if (inst.finalize(axil_interconnect_modules[0], json, 3)) {
        JSON_POST_MSG(3, "Probes:");
        for (auto& sig : inst.probes) {
          JSON_POST_MSG(4, "--> %s", sig.fullname.c_str());
          JSON_POST_MSG(5, ": %s (width=%d, offset=%d)", sig.name.c_str(), sig.width, sig.offset);
        }
        JSON_POST_MSG(3, "Trigger Inputs:");
        for (auto& sig : inst.trigger_inputs) {
          JSON_POST_MSG(4, "--> %s", sig.fullname.c_str());
          JSON_POST_MSG(5, ": %s (width=%d, offset=%d)", sig.name.c_str(), sig.width, sig.offset);
        }
        JSON_POST_MSG(3, "AXIL AWReady:");
        JSON_POST_MSG(4, "--> %s (width=%d, offset=%d)", inst.awready.name.c_str(), inst.awready.width, inst.awready.offset);
        for (auto& cs : inst.connection_chain) {
          JSON_POST_MSG(5, ": %s", cs.c_str());
        }
        JSON_POST_MSG(3, "Connected to AXIL Interconnect #%d", inst.index);
        ocla_count++;
      } else {
        JSON_POST_MSG(3, "Error: Disqualify this instance");
      }
    }
ANALYZE_MSG_END:
    json << "    \"End of OCLA Analysis\"\n  ]";
    if (ocla_count) {
      json << ",\n  \"ocla\" : [\n";
      uint32_t index = 0;
      for (auto& inst : ocla_instantiators) {
        if (inst.status) {
          json << "    {\n",
          json_write_param(inst.ocla, json, 3);
          json << gen_string(",\n      \"addr\" : %d", inst.addr).c_str();
          json_write_signals("probes", inst.probes, json);
          index++;
          if (index < ocla_count) {
            json << "    },\n";
          } else {
            json << "    }\n";
          }
        }
      }
      json << "  ]";
      json << ",\n  \"axil\" : {\n";
      json_write_param(axil_interconnect_modules[0], json, 2);
      json << "\n  }";
    }
    while (ocla_modules.size()) {
      delete ocla_modules.back();
      ocla_modules.pop_back();
    }
    while (axil_interconnect_modules.size()) {
      delete axil_interconnect_modules.back();
      axil_interconnect_modules.pop_back();
    }
    json << "\n}\n";
  }
 private:
  static void dump_const(std::ostringstream &f, const RTLIL::Const &data, int width = -1, int offset = 0, bool autoint = true) {
    if (width < 0) {
      width = data.bits.size() - offset;
    }
    if ((data.flags & RTLIL::CONST_FLAG_STRING) == 0 || width != (int)data.bits.size()) {
      if (width == 32 && autoint) {
        int32_t val = 0;
        for (int i = 0; i < width; i++) {
          log_assert(offset+i < (int)data.bits.size());
          switch (data.bits[offset+i]) {
            case State::S0: break;
            case State::S1: val |= 1 << i; break;
            default: val = -1; break;
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
        for (int i = offset+width-1; i >= offset; i--) {
          log_assert(i < (int)data.bits.size());
          switch (data.bits[i]) {
            case State::S0: f << stringf("0"); break;
            case State::S1: f << stringf("1"); break;
            case RTLIL::Sx: f << stringf("x"); break;
            case RTLIL::Sz: f << stringf("z"); break;
            case RTLIL::Sa: f << stringf("-"); break;
            case RTLIL::Sm: f << stringf("m"); break;
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
  static void dump_sigspec(std::ostringstream &f, std::vector<OCLA_SIGNAL>& ss, const RTLIL::SigSpec &sig, bool autoint=true) {
    if (sig.is_chunk()) {
      OCLA_SIGNAL s = dump_sigchunk(f, sig.as_chunk(), autoint);
      ss.push_back(s);
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
  static OCLA_SIGNAL dump_sigchunk(std::ostringstream &f, const RTLIL::SigChunk &chunk, bool autoint) {
    std::ostringstream temp;
    std::string name = "";
    bool show_index = false;
    if (chunk.wire == NULL) {
      dump_const(temp, chunk.data, chunk.width, chunk.offset, autoint);
      name = temp.str();
    } else {
      name = chunk.wire->name.c_str();
      show_index = !(chunk.width == chunk.wire->width && chunk.width == 1 && chunk.offset == 0);
      if (chunk.width == chunk.wire->width && chunk.offset == 0) {
        temp << stringf("%s", chunk.wire->name.c_str());
      } else if (chunk.width == 1) {
        temp << stringf("%s [%d]", chunk.wire->name.c_str(), chunk.offset);
      } else {
        temp << stringf("%s [%d:%d]", chunk.wire->name.c_str(), chunk.offset+chunk.width-1, chunk.offset);
      }
    }
    f << temp.str().c_str();
    return OCLA_SIGNAL(temp.str(), name, (uint32_t)(chunk.width), (uint32_t)(chunk.offset), show_index);
  }
  static void get_modules(RTLIL::Design* design, std::vector<OCLA_MODULE*>& ocla_modules, 
                          std::vector<AXIL_INTERCONNECT_MODULE*>& axil_interconnect_modules,
                          std::ofstream& json) {
    for (auto module : design->modules()) {
      if (module->name == "\\ocla" ||
          (module->name.size() > 5 && module->name.substr(module->name.size() - 5) == "\\ocla")) {
        printf("OCLA Module: %s\n", module->name.c_str());
        JSON_POST_MSG(0, "Detected Potential OCLA: %s", module->name.c_str());
        OCLA_MODULE* m = new (OCLA_MODULE)(module->name.c_str());
        log_assert(m != nullptr);
        MODULE_IP* ip = (MODULE_IP*)(m);
        get_module_params(module, ip, json);
        if (ip != nullptr && m->check_type(json)) {
          ocla_modules.push_back(m);
          JSON_POST_MSG(1, "Qualified as OCLA module");
        } else {
          JSON_POST_MSG(1, "Error: this is not qualified as OCLA module");
        }
      } else if (module->name == "\\axil_interconnect" ||
                (module->name.size() > 18 && module->name.substr(module->name.size() - 18) == "\\axil_interconnect")) {
        printf("AXIL Interconnect Module: %s\n", module->name.c_str());
        JSON_POST_MSG(0, "Detected Potential AXIL Interconnect: %s", module->name.c_str());
        AXIL_INTERCONNECT_MODULE* m = new (AXIL_INTERCONNECT_MODULE)(module->name.c_str());
        log_assert(m != nullptr);
        MODULE_IP* ip = (MODULE_IP*)(m);
        get_module_params(module, ip, json);
        if (ip != nullptr && m->check_type(json)) {
          axil_interconnect_modules.push_back(m);
          JSON_POST_MSG(1, "Qualified as AXIL Interconnect module");
        } else {
          JSON_POST_MSG(1, "Error: this is not qualified as AXIL Interconnect module");
        }
      }
    }
  }
  static bool check_unique_axil_interconnect(RTLIL::Design* design, std::string module_name, 
                                              std::string& connection_name, std::ofstream& json) {
    bool status = true;
    RTLIL::Module* top_module = design->top_module();
    JSON_POST_MSG(0, "Check uniqueness of AXIL Interconnect");
    int level = 0;
    connection_name = "";
    while (status) {
      JSON_POST_MSG(1, "Module: %s", module_name.c_str());
      std::vector<std::string> module_names;
      for (auto m : design->modules()) {
        for (auto cell : m->cells()) {
          if (std::string(cell->type.c_str()) == module_name) {
            JSON_POST_MSG(2, "Instantiated by %s as %s", m->name.c_str(), cell->name.c_str());
            module_names.push_back(m->name.c_str());
            if (level) {
              if (connection_name.size()) {
                connection_name = gen_string("%s.%s", cell->name.c_str(), connection_name.substr(1).c_str());
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
            JSON_POST_MSG(3, "Connection chain for AXIL Interconnect: %s", connection_name.c_str());
          } else {
            JSON_POST_MSG(3, "Hierarchy level for AXIL Interconnect is out of expectation");
            status = false;
          }
          break;
        }
      } else {
        status = false;
      }
    }
    return status;
  }
  static void get_module_params(RTLIL::Module* module, MODULE_IP*& ip, std::ofstream& json) {
    for (const auto &p : module->avail_parameters) {
      const auto &it = module->parameter_default_values.find(p);
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
  static void get_ocla_instantiator(RTLIL::Design* design, OCLA_MODULE* module, 
                                    std::vector<std::string>& instantiators,
                                    std::vector<OCLA_MODULE*>& instantiated_modules,
                                    std::ofstream& json) {
    bool found = false;
    JSON_POST_MSG(0, "Check instantiator for OCLA module %s", module->name.c_str());
    for (auto m : design->modules()) {
      for (auto cell : m->cells()) {
        if (std::string(cell->type.c_str()) == module->name) {
          JSON_POST_MSG(1, "Instantiated by %s", m->name.c_str());
          instantiators.push_back(std::string(m->name.c_str()));
          instantiated_modules.push_back(module);
          found = true;
        }        
      }
    }
    if (!found) {
      JSON_POST_MSG(1, "Warning: Does not detect any instantiator");
    }
  }
  static void get_ocla_instantiator(RTLIL::Module* top_module, std::string instantiator_module, 
                                    OCLA_MODULE* module, std::vector<OCLA_INSTANTIATOR>& instantiators, 
                                    std::string connection_name, std::ofstream& json) {
    bool found = false;
    JSON_POST_MSG(0, "Restrive OCLA information: %s", instantiator_module.c_str());
    for (auto cell : top_module->cells()) {
      if (std::string(cell->type.c_str()) == instantiator_module) {
        JSON_POST_MSG(1, "Instantiated as %s", cell->name.c_str());
        std::vector<OCLA_SIGNAL> probes;
        std::vector<OCLA_SIGNAL> input_triggers;
        std::vector<OCLA_SIGNAL> awready;
        for (auto &connection : cell->connections()) {
          std::ostringstream wire;
          if (std::string(connection.first.c_str()) == "\\i_probes") {
            dump_sigspec(wire, probes, connection.second);
            JSON_POST_MSG(2, "\\i_probes connected to %s", wire.str().c_str());
          } else if (std::string(connection.first.c_str()) == "\\i_trigger_input") {
            dump_sigspec(wire, input_triggers, connection.second);
            JSON_POST_MSG(2, "\\i_trigger_inputs connected to %s", wire.str().c_str());
          } else if (std::string(connection.first.c_str()) == "\\s_axil_awready") {
            dump_sigspec(wire, awready, connection.second);
            JSON_POST_MSG(2, "\\s_axil_awready connected to %s", wire.str().c_str());
          }
        }
        if (probes.size() > 0 && awready.size() == 1) {
          std::vector<std::string> connections;
          int index = search_axil_interconnection(top_module, connection_name, awready[0].fullname, 
                                                  connections, json);
          if (index >= 0) {
            instantiators.push_back(OCLA_INSTANTIATOR(instantiator_module, 
                                                      cell->name.c_str(), 
                                                      module,
                                                      probes, 
                                                      input_triggers, 
                                                      awready[0],
                                                      (uint32_t)(index),
                                                      connections));
            found = true;
          } else {
            JSON_POST_MSG(2, "Error: Could not find AXIL Interconnect connection chain for %s", 
                              awready[0].fullname.c_str());
          }
        } else {
          JSON_POST_MSG(2, "Error: Probes count=%ld, AWReady count=%ld (bus=%d)", 
                        probes.size(), awready.size(), awready.size() ? awready[0].width : -1);
        }
      }
    }
    if (!found) {
      JSON_POST_MSG(1, "Warning: Does not detect OCLA instantiator instantiated by top module or there is error in detecting signals");
    }
  }
  static int search_axil_interconnection(RTLIL::Module* top_module, std::string connection_name,
                                          std::string awready, std::vector<std::string>& connections,
                                          std::ofstream& json) {
    JSON_POST_MSG(2, "Searching AXIL Interconnect connection for %s", awready.c_str());
    JSON_POST_MSG(3, "Expected end connection chain: %s", connection_name.c_str());
    int index = -1;
    int iteration = 0;
    std::string src = awready;
    bool found = true;
    while (index == -1 && found && iteration < 100) {
      found = false;
      std::vector<OCLA_SIGNAL> temp;
      std::ostringstream connection;
      for (auto iter = top_module->connections().begin(); iter != top_module->connections().end(); ++iter) {
        temp.clear();
        connection.str("");
        connection.clear();
        dump_sigspec(connection, temp, iter->second);
        if (std::string(connection.str()) == src) {
          temp.clear();
          connection.str("");
          connection.clear();
          dump_sigspec(connection, temp, iter->first);
          src = std::string(connection.str());
          connections.push_back(src);
          JSON_POST_MSG(4, "-> %s", src.c_str());
          found = true;
          break;
        }
      }
      for (int i = 0; i < 16 && found; i++) {
        std::string stop_signal = gen_string("%s.m%02d_axil_awready", connection_name.c_str(), i);
        if (stop_signal == src) {
          index = i;
          JSON_POST_MSG(5, "Found complete connection chain at index %d", index);
          break;
        }
      }
      iteration++;
    }
    return index;
  }
  static void json_write_param(MODULE_IP* ip, std::ofstream& json, uint32_t space) {
    std::string info = "";
    size_t index = 0;
    for (auto& p : ip->params) {
      for (uint32_t i = 0; i < space; i++) {
        json << "  ";
      }
      if (p.second.is_str) {
        std::string* ptr = (std::string*)(p.second.ptr);
        info = gen_string("\"%s\" : \"%s\"", p.first.c_str(), ptr->c_str());
      } else {
        uint32_t* ptr = (uint32_t*)(p.second.ptr);
        info = gen_string("\"%s\" : %d", p.first.c_str(), *ptr);
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
  static void json_write_signals(std::string name, std::vector<OCLA_SIGNAL>& signals, std::ofstream& json) {
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
        json << gen_string("        \"%s[%d]\"", s.name.c_str(), s.offset).c_str();
      } else {
        json << gen_string("        \"%s[%d:%d]\"", s.name.c_str(), s.offset + s.width - 1, s.offset).c_str();
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

  OCLA_AnalyzerPass() : Pass("ocla_analyze", "Analyze OCLA information from the design for Raptor") { }

  void help() override
  {
    log("\n");
    log("    ocla_analyze\n");
    log("\n");
    log("Analyze OCLA information from the design for Raptor and write out 'ocla.json'\n");
    log("\n");
    log("    -top <top_module_name>\n");
    log("       performs Analyze from the top module with name 'top_module_name'.\n");
    log("    -auto-top \n");
    log("       detects automatically the top module. If several tops, it picks up the one with deepest hierarchy. Analyze from this selected top module.\n");
    log("\n");
  }

  void execute(std::vector<std::string> args, RTLIL::Design *design) override
  {
    // Parse Analyze command arguments
    std::string top_name = "";
    bool is_auto = false;
    size_t argidx;
    for (argidx = 1; argidx < args.size(); argidx++)
    {
      if (args[argidx] == "-top" && argidx+1 < args.size()) {
        top_name = args[++argidx];
        continue;
      }
      if (args[argidx] == "-auto-top") {
        is_auto = true;
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
    std::ofstream json("ocla.json");
    OCLA_Analyzer::analyze(design, json);
    json.close();
  }
} OCLA_AnalyzerPass;

PRIVATE_NAMESPACE_END
