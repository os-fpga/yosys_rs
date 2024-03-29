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

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct TribufConfig {
	bool merge_mode;
	bool rs_merge_mode;
	bool logic_mode;
	bool rs_logic_mode;
	bool formal_mode;

	TribufConfig() {
		merge_mode = false;
		rs_merge_mode = false;
		logic_mode = false;
		rs_logic_mode = false;
		formal_mode = false;
	}
};

struct TribufWorker {
	Module *module;
	SigMap sigmap;
	const TribufConfig &config;

	TribufWorker(Module *module, const TribufConfig &config) : module(module), sigmap(module), config(config)
	{
	}

	static bool is_all_z(SigSpec sig)
	{
		for (auto bit : sig)
			if (bit != State::Sz)
				return false;
		return true;
	}

	void run()
	{
		dict<SigSpec, vector<Cell*>> tribuf_cells;
		pool<SigBit> output_bits;

		if (config.logic_mode || config.rs_logic_mode || config.formal_mode)
			for (auto wire : module->wires())
				if (wire->port_output)
					for (auto bit : sigmap(wire))
						output_bits.insert(bit);

		for (auto cell : module->selected_cells())
		{
			if (cell->type == ID($tribuf))
				tribuf_cells[sigmap(cell->getPort(ID::Y))].push_back(cell);

			if (cell->type == ID($_TBUF_))
				tribuf_cells[sigmap(cell->getPort(ID::Y))].push_back(cell);

			if (cell->type.in(ID($mux), ID($_MUX_)))
			{
				IdString en_port = cell->type == ID($mux) ? ID::EN : ID::E;
				IdString tri_type = cell->type == ID($mux) ? ID($tribuf) : ID($_TBUF_);

				if (is_all_z(cell->getPort(ID::A)) && is_all_z(cell->getPort(ID::B))) {
					module->remove(cell);
					continue;
				}

				if (is_all_z(cell->getPort(ID::A))) {
					cell->setPort(ID::A, cell->getPort(ID::B));
					cell->setPort(en_port, cell->getPort(ID::S));
					cell->unsetPort(ID::B);
					cell->unsetPort(ID::S);
					cell->type = tri_type;
					tribuf_cells[sigmap(cell->getPort(ID::Y))].push_back(cell);
					module->design->scratchpad_set_bool("tribuf.added_something", true);

	                                if (config.rs_merge_mode || config.rs_logic_mode) {
                                           std::string inst_name= log_id(cell->name);
                                           std::regex pattern1("\\$tribuf_conflict\\$");
                                           std::regex pattern2("\\$\\d+$");
                                           std::regex pattern3(".*\\$");
                                           std::string out = std::regex_replace(inst_name,pattern1,"");
                                           out = std::regex_replace(out,pattern2,"");
                                           out = std::regex_replace(out,pattern3,"");
                
                                           log_warning("Transforming tri-state at RTL line %s into pure logic:\n", 
                                                       out.c_str());
                                           log("         Functional Behavior may change.\n");
                                        }
					continue;
				}

				if (is_all_z(cell->getPort(ID::B))) {
					cell->setPort(en_port, module->Not(NEW_ID, cell->getPort(ID::S)));
					cell->unsetPort(ID::B);
					cell->unsetPort(ID::S);
					cell->type = tri_type;
					tribuf_cells[sigmap(cell->getPort(ID::Y))].push_back(cell);
					module->design->scratchpad_set_bool("tribuf.added_something", true);

	                                if (config.rs_merge_mode || config.rs_logic_mode) {
                                           std::string inst_name= log_id(cell->name);
                                           std::regex pattern1("\\$tribuf_conflict\\$");
                                           std::regex pattern2("\\$\\d+$");
                                           std::regex pattern3(".*\\$");
                                           std::string out = std::regex_replace(inst_name,pattern1,"");
                                           out = std::regex_replace(out,pattern2,"");
                                           out = std::regex_replace(out,pattern3,"");
                
                                           log_warning("Transforming tri-state at RTL line %s into pure logic:\n", 
                                                       out.c_str());
                                           log("         Functional Behavior may change.\n");
                                        }
					continue;
				}
			}
		}

		if (config.merge_mode || config.rs_merge_mode || config.logic_mode || config.rs_logic_mode ||
                    config.formal_mode)
		{
			for (auto &it : tribuf_cells)
			{
				bool no_tribuf = false;

				if ((config.logic_mode || config.rs_logic_mode) && !config.formal_mode) {
					no_tribuf = true;
					for (auto bit : it.first)
						if (output_bits.count(bit))
							no_tribuf = false;
				}

				if (config.formal_mode)
					no_tribuf = true;

				if (GetSize(it.second) <= 1 && !no_tribuf)
					continue;

				if (config.formal_mode && GetSize(it.second) >= 2) {
					for (auto cell : it.second) {
						SigSpec others_s;

						for (auto other_cell : it.second) {
							if (other_cell == cell)
								continue;
							else if (other_cell->type == ID($tribuf))
								others_s.append(other_cell->getPort(ID::EN));
							else
								others_s.append(other_cell->getPort(ID::E));
						}

						auto cell_s = cell->type == ID($tribuf) ? cell->getPort(ID::EN) : cell->getPort(ID::E);

						auto other_s = module->ReduceOr(NEW_ID, others_s);

						auto conflict = module->And(NEW_ID, cell_s, other_s);

						std::string name = stringf("$tribuf_conflict$%s", log_id(cell->name));
						auto assert_cell = module->addAssert(name, module->Not(NEW_ID, conflict), SigSpec(true));

						assert_cell->set_src_attribute(cell->get_src_attribute());
						assert_cell->set_bool_attribute(ID::keep);

						module->design->scratchpad_set_bool("tribuf.added_something", true);
					}
				}

				SigSpec pmux_b, pmux_s, muxout;
				for (auto cell : it.second) {
					if (cell->type == ID($tribuf)) 
						pmux_s.append(cell->getPort(ID::EN));
					else
						pmux_s.append(cell->getPort(ID::E));
					pmux_b.append(cell->getPort(ID::A));
					module->remove(cell);
				}

                                // in Rapid Silicon mode we consider also pmux with size 1 and during merge
                                // we build a pmux with default value '0' instead of default 'x'.
                                //
		                if (config.rs_merge_mode || config.rs_logic_mode) {
				   muxout = GetSize(pmux_s) >= 1 ? module->Pmux(NEW_ID, SigSpec(State::S0, GetSize(it.first)), pmux_b, pmux_s) : pmux_b;
                                } else {
				   muxout = GetSize(pmux_s) > 1 ? module->Pmux(NEW_ID, SigSpec(State::Sx, GetSize(it.first)), pmux_b, pmux_s) : pmux_b;
                                }

				if (no_tribuf)
					module->connect(it.first, muxout);
				else {
					module->addTribuf(NEW_ID, muxout, module->ReduceOr(NEW_ID, pmux_s), it.first);
					module->design->scratchpad_set_bool("tribuf.added_something", true);
				}
			}
		}
	}
};

struct TribufPass : public Pass {
	TribufPass() : Pass("tribuf", "infer tri-state buffers") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    tribuf [options] [selection]\n");
		log("\n");
		log("This pass transforms $mux cells with 'z' inputs to tristate buffers.\n");
		log("\n");
		log("    -merge\n");
		log("        merge multiple tri-state buffers driving the same net\n");
		log("        into a single buffer.\n");
		log("\n");
		log("    -logic\n");
		log("        convert tri-state buffers that do not drive output ports\n");
		log("        to non-tristate logic. this option implies -merge.\n");
		log("\n");
		log("    -formal\n");
		log("        convert all tri-state buffers to non-tristate logic and\n");
		log("        add a formal assertion that no two buffers are driving the\n");
		log("        same net simultaneously. this option implies -merge.\n");
		log("\n");

                // do not list -rs_merge and -rs_logic as they are Rapid Silicon hidden options
                //
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		TribufConfig config;

		log_header(design, "Executing TRIBUF pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-merge") {
				config.merge_mode = true;
				continue;
			}
			if (args[argidx] == "-rs_merge") {
				config.rs_merge_mode = true;
				continue;
			}
			if (args[argidx] == "-logic") {
				config.logic_mode = true;
				continue;
			}
			if (args[argidx] == "-rs_logic") {
				config.rs_logic_mode = true;
				continue;
			}
			if (args[argidx] == "-formal") {
				config.formal_mode = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules()) {
			TribufWorker worker(module, config);
			worker.run();
		}
	}
} TribufPass;

PRIVATE_NAMESPACE_END
