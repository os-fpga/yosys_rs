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

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/cellaigs.h"
#include "kernel/log.h"
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct AnlzWriter
{
	std::ostream &f;
	bool use_selection;
	bool aig_mode;
	bool compat_int_mode;

	Design *design;
	Module *module;

	pool<Aig> aig_models;

	AnlzWriter(std::ostream &f, bool use_selection, bool aig_mode, bool compat_int_mode) :
			f(f), use_selection(use_selection), aig_mode(aig_mode),
			compat_int_mode(compat_int_mode) { }

	string get_string(string str)
	{
		string newstr = "\"";
		for (char c : str) {
			if (c == '\\')
				newstr += "\\\\";
			else if (c == '"')
				newstr += "\\\"";
			else if (c == '\b')
				newstr += "\\b";
			else if (c == '\f')
				newstr += "\\f";
			else if (c == '\n')
				newstr += "\\n";
			else if (c == '\r')
				newstr += "\\r";
			else if (c == '\t')
				newstr += "\\t";
			else if (c < 0x20)
				newstr += stringf("\\u%04X", c);
			else
				newstr += c;
		}
		return newstr + "\"";
	}

	string get_name(IdString name)
	{
		return get_string(RTLIL::unescape_id(name));
	}

	void dump_parameter_value(const Const &value)
	{
		if ((value.flags & RTLIL::ConstFlags::CONST_FLAG_STRING) != 0) {
			string str = value.decode_string();
			int state = 0;
			for (char c : str) {
				if (state == 0) {
					if (c == '0' || c == '1' || c == 'x' || c == 'z')
						state = 0;
					else if (c == ' ')
						state = 1;
					else
						state = 2;
				} else if (state == 1 && c != ' ')
					state = 2;
			}
			if (state < 2)
				str += " ";
			f << get_string(str);
		} else if (compat_int_mode && GetSize(value) <= 32 && value.is_fully_def()) {
			if ((value.flags & RTLIL::ConstFlags::CONST_FLAG_SIGNED) != 0)
				f << stringf("%d", value.as_int());
			else
				f << stringf("%u", value.as_int());
		} else {
			f << get_string(value.as_string());
		}
	}

        void dump_parameters(const dict<IdString, Const> &parameters)
        {
                bool first = true;
                for (auto &param : parameters) {
                        
                        if (!first) {
                          f << stringf(",\n");
                        }
                        f << stringf("              {\n");
                        f << stringf("                  \"name\": %s\n", get_name(param.first).c_str());
                        f << stringf("                  \"value\": ");

                        dump_parameter_value(param.second);

                        f << stringf(" \n              }");

                        first = false;
                }
                f << stringf("\n");
        }


        void dump_parameters(Module* module)
        {
                if (module->parameter_default_values.size()) {
                   f << stringf("          \"parameters\": [\n");
                   dump_parameters(module->parameter_default_values);
                   f << stringf("          ],\n");
                }
        }


	void dump_wire_info(Wire* w)
	{
                int is_reg = 0;

                int lsb = 0;
                int msb = 0;

                if (w->upto) {
                   msb = w->start_offset;
                   lsb = msb + w->width - 1;

                } else {
                   lsb = w->start_offset;
                   msb = lsb + w->width - 1;
                }

                f << stringf("                  \"name\": %s,\n",get_name(w->name).c_str());
                f << stringf("                  \"range\": {\n");
                f << stringf("                      \"lsb\": %d,\n", lsb);
                f << stringf("                      \"msb\": %d\n", msb);
                f << stringf("                  },\n");

                if (is_reg) {
                   f << stringf("                  \"type\": \"REG\"\n");
                } else {
                   f << stringf("                  \"type\": \"LOGIC\"\n");
                }
        }

	void dump_internalSignals(Module *module)
	{
                int anySignal = 0;

		for (auto w : module->wires()) {

                   if (w->port_input) {
                     continue;
                   }

                   if (w->port_output) {
                     continue;
                   }

                   if (w->name[0] == '$') {
                     continue;
                   }

                   anySignal++;
                   break;
                }

                if (!anySignal) {
                   return;
                }

                f << stringf("          \"internalSignals\": [\n");

                int first = 1;

		for (auto w : module->wires()) {

                   if (w->port_input) {
                     continue;
                   }

                   if (w->port_output) {
                     continue;
                   }

                   if (!first) {
                     f << stringf(",\n");
                   }
                   // do not dump internal wires
                   //
                   if (w->name[0] == '$') {
                     continue;
                   }

                   f << stringf("              {\n");
                   dump_wire_info(w);
                   f << stringf("              }");

                   first = 0;
                }

                f << stringf("\n          ],\n");
        }

	void dump_moduleInsts(Module *module)
	{
                int anyInst = 0;

		for (auto c : module->cells()) {

                   if (c->name[0] == '$') {
                      continue;
                   }
                   anyInst++;
                   break;
                }

                if (!anyInst) {
                  return;
                }

                f << stringf("          \"moduleInsts\": [\n");

                int first = 1;
		for (auto c : module->cells()) {

                   if (c->name[0] == '$') {
                      continue;
                   }

                   if (!first) {
                      f << stringf(",\n");
                   }
                   f << stringf("              {\n");
                   f << stringf("                   \"instName\": %s,\n", get_name(c->name).c_str());
                   f << stringf("                   \"module\":  %s,\n", get_name(c->type).c_str());
                   f << stringf("                   \"parameters\": []\n");
                   f << stringf("              }");

                   first = 0;
                }

                f << stringf("\n          ],\n");
        }

	void dump_ports(Module *module)
	{
		log_assert(module->design == design);

                f << stringf("          \"ports\": [\n");
                bool first = true;
                for (auto n : module->ports) {

                        Wire *w = module->wire(n);

                        if (!first) {
                           f << stringf(",\n");
                        }
                        f << stringf("              {\n");
                        f << stringf("                  \"direction\": \"%s\",\n", 
                             w->port_input ? w->port_output ? "Inout" : "Input" : "Output");

                        dump_wire_info(w);

                        f << stringf("              }");

                        first = false;
                }
                f << stringf("\n          ]");
        }

        void dump_fileIDs()
        {
		f << stringf("  \"fileIDs\": {\n");
		f << stringf("  },\n");
        }

        void dump_module(Module* module, int dump_name)
        {

                // write file ID
                //

                // write internalSignals
                //
	        dump_internalSignals(module);

                // write language
                //

                // write line
                //
                     
                if (dump_name) {
                  f << stringf("          \"module\": %s,\n", get_name(module->name).c_str());
                }

                // write moduleInsts
                //
	        dump_moduleInsts(module);

                // write parameters
                //
                dump_parameters(module);

                // write ports
                //
                dump_ports(module);

        }
   
        void dump_hierTree()
        {
                Module *topmod = design->top_module();

                if (!topmod) {
                   log_error("Cannot find top module ! (please run 'hierarchy auto-top' before)\n");
                }

                f << stringf("  \"hierTree\": [\n");
                f << stringf("      {\n");

                dump_module(topmod, 0);

                f << stringf(",\n");

                // write topModule
                //
                f << stringf("          \"topModule\": %s", get_name(topmod->name).c_str());

                f << stringf("\n      }\n");
                f << stringf("  ],\n");
        }

	void dump_modules(Module* topModule)
	{
                f << stringf("  \"modules\": {\n");
                vector<Module*> modules = design->modules();

                bool first_module = true;

                for (auto module : modules) {

                     if (module == topModule) {
                        continue;
                     }

                     if (!first_module) {
                        f << stringf(",\n");
                     }
                     f << stringf("      %s: {\n", get_name(module->name).c_str());
                     //f << stringf("          \"module\": %s,\n", get_name(module->name).c_str());

                     dump_module(module, 1);

                     f << stringf("\n      }");

                     first_module = false;
                }

                f << stringf("\n  }");

        }

	void dump_port_info(Design *design_)
	{
		design = design_;
		design->sort();

                Module *topmod = design->top_module();

                if (!topmod) {
                   log_error("Cannot find top module ! (please run 'hierarchy auto-top' before)\n");
                }

		f << stringf("[");
                f << stringf("\n      {\n");

                dump_ports(topmod);

                f << stringf(",\n");

                f << stringf("          \"topModule\": %s", get_name(topmod->name).c_str());

                f << stringf("\n      }\n");
                f << stringf("]\n");
        }

	void dump_hier_info(Design *design_)
	{
		design = design_;
		design->sort();

                Module *topmod = design->top_module();

                if (!topmod) {
                   log_error("Cannot find top module ! (please run 'hierarchy auto-top' before)\n");
                }

		f << stringf("{\n");

                dump_fileIDs();

                dump_hierTree();

                dump_modules(topmod);

		f << stringf("\n}\n");
	}

};

struct AnlzPass : public Pass {

	AnlzPass() : Pass("analyze", "write design into two JSON files for Raptor") { }

	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    analyze\n");
		log("\n");
		log("Write two JSON files 'hier_info.json' and 'port_info.json' of the current design.\n");
		log("\n");
		log("    -top <top_module_name>\n");
		log("       performs Analyze from the top module with name 'top_module_name'.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (design->is_protected_rtl()){
			log_warning("Dumping JSON file is not supported in case of encrypted RTL\n");
			return;
		}

		std::string hier_filename = "hier_info.json";
		std::string port_filename = "port_info.json";

		std::string top_name = "";

		bool aig_mode = false;
		bool compat_int_mode = false;

                // Parse Analyze command arguments
                //
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
                   if (args[argidx] == "-top" && argidx+1 < args.size()) {
                        top_name = args[++argidx];
                        continue;
                   }
                   log_error("Analyze Unknow Option : \"%s\"\n", args[argidx].c_str());
		}
		extra_args(args, argidx, design);

                // if no top module specified then pick up the one with "hierarchy-auto-top"
                //
                if (top_name.length() == 0) {

                  run_pass("hierarchy -auto-top");

                } else { // if one specified , then give it to "hierarchy -top"
                         // "hierarchy" command will fail if there is no such top_name

                  string cmd = "hierarchy -top " + top_name;
                  run_pass(cmd.c_str());
                }
                     
		std::ostream *f;

                // Dumping "hier_info.js" file
                //
		std::ofstream *ff = new std::ofstream;
		ff->open(hier_filename.c_str(), std::ofstream::trunc);

		if (ff->fail()) {
		   delete ff;
		   log_error("Can't open file `%s' for writing: %s\n", 
                             hier_filename.c_str(), strerror(errno));
		}

		f = ff;

                log("\nDumping file %s ...\n", hier_filename.c_str());

		AnlzWriter anlz_writer(*f, true, aig_mode, compat_int_mode);
		anlz_writer.dump_hier_info(design);

		delete f;

                // Dumping "port_info.js" file
                //
                ff = new std::ofstream;
                ff->open(port_filename.c_str(), std::ofstream::trunc);

                if (ff->fail()) {
                   delete ff;
                   log_error("Can't open file `%s' for writing: %s\n",
                             port_filename.c_str(), strerror(errno));
                }

                f = ff;

                log("Dumping file %s ...\n", port_filename.c_str());

                AnlzWriter anlz_writer_port(*f, true, aig_mode, compat_int_mode);
                anlz_writer_port.dump_port_info(design);

                delete f;

	}
} AnlzPass;

PRIVATE_NAMESPACE_END
