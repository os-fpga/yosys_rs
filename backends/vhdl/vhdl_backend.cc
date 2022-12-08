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
 *  ---
 *
 *  A simple and straightforward Vhdl backend.
 *
 */

/* Copyright (C) Rapid Silicon
 *
 * This is a specific code from rapid Silicon to handle VHDL netlist output.
 * It is a copy/paste of the Verilog version (yosys/backends/verilog) and 
 * only the "structural" parts have been modified to allow the VHDL ouptut of structural netlist.
 * It is currently customized for specific cells which have been hard-coded like :
 * shr, adder_carry, TDP36K, RS_DSP2_MULT, sh_dff, latchsre, dffnsre, dffsre. 
 * Things are generally straighforward when coming from Verilog code except when we want to adress 
 * port map association of an instance like :
 *     Cell (...
 *           o1 => (s1 & s2 & s3)
 *   where 'o1' is the actual output of a cell instance and '(s1 & s2 & s3)' a complex 
 *   expression associated to this actual 'o1'. It is ok in Verilog but not in VHDL.
 *   In VHDL we have to do something like :
 *   o1 => s_o1
 *   s1 <= s_o1 (a1 downto/upto b1)
 *   s2 <= s_o1 (a2 downto/upto b2)
 *   s3 <= s_o1 (a3 downto/upto b3)
 *
 * [Thierry]
 */

#include "kernel/register.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"
#include "kernel/mem.h"
#include <string>
#include <sstream>
#include <set>
#include <map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

bool verbose, enableopt, norename, noattr, attr2comment, noexpr, nodec, nohex, nostr, extmem, defparam, decimal, siminit, systemvhdl, simple_lhs;
int auto_name_counter, auto_name_offset, auto_name_digits, extmem_counter;
std::map<RTLIL::IdString, int> auto_name_map;
std::set<RTLIL::IdString> reg_wires;
std::string auto_prefix, extmem_prefix;

RTLIL::Module *active_module;
dict<RTLIL::SigBit, RTLIL::State> active_initdata;
SigMap active_sigmap;
IdString initial_id;

void reset_auto_counter_id(RTLIL::IdString id, bool may_rename)
{
	const char *str = id.c_str();

	if (*str == '$' && may_rename && !norename)
		auto_name_map[id] = auto_name_counter++;

	if (str[0] != '\\' || str[1] != '_' || str[2] == 0)
		return;

	for (int i = 2; str[i] != 0; i++) {
		if (str[i] == '_' && str[i+1] == 0)
			continue;
		if (str[i] < '0' || str[i] > '9')
			return;
	}

	int num = atoi(str+2);
	if (num >= auto_name_offset)
		auto_name_offset = num + 1;
}

void reset_auto_counter(RTLIL::Module *module)
{
	auto_name_map.clear();
	auto_name_counter = 0;
	auto_name_offset = 0;

	reset_auto_counter_id(module->name, false);

	for (auto w : module->wires())
		reset_auto_counter_id(w->name, true);

	for (auto cell : module->cells()) {
		reset_auto_counter_id(cell->name, true);
		reset_auto_counter_id(cell->type, false);
	}

	for (auto it = module->processes.begin(); it != module->processes.end(); ++it)
		reset_auto_counter_id(it->second->name, false);

	auto_name_digits = 1;
	for (size_t i = 10; i < auto_name_offset + auto_name_map.size(); i = i*10)
		auto_name_digits++;

	if (verbose)
		for (auto it = auto_name_map.begin(); it != auto_name_map.end(); ++it)
			log("  renaming `%s' to `%s_%0*d_'.\n", it->first.c_str(), auto_prefix.c_str(), auto_name_digits, auto_name_offset + it->second);
}

std::string next_auto_id()
{
	return stringf("%s_%0*d_", auto_prefix.c_str(), auto_name_digits, auto_name_offset + auto_name_counter++);
}

// -----------------------------------------
// Thierry : taking care of legalizing names for VHDL with existing verilog code
//
vector<string*> g_allNewStrings;

// Cleanup by deleting string creating with "new" in the "id(...)" function
//
void cleanUpNewStrings()
{
   for (auto it = g_allNewStrings.begin(); it != g_allNewStrings.end(); it++) {
     delete(*it);
   }
   g_allNewStrings.clear();
}

std::string legalizeVhdlName(const char* str)
{
	bool firstChar = true;

        // Thierry:
        // For VHDL we need to remove/change some forbiden chars
        // We need to create a new string and return it through this function
        // We need to delete these new created strings stored in 'g_allNewStrings' later
        //
        string* s = new string;
        g_allNewStrings.push_back(s);

        // Thierry
        //
        for (long unsigned int i = 0; str[i]; i++)
        {
                if (i == strlen(str)) {
                  break;
                }
                if (str[i] == '$') {
                  continue;
                }
                if (str[i] == '\\') {
                  continue;
                }
                if (str[i] == '[') {
                  continue;
                }
                if (str[i] == ']') {
                  continue;
                }
                if (str[i] == '.') {
                  continue;
                }
                if (str[i] == '_') {
                  if (firstChar) {
                    *s += "x_";
                    firstChar = false;
                    continue;
                  }
                  if (i == strlen(str)-1) {
                    continue;
                  }
                }
                firstChar = false;
                *s += str[i];
        }
        return *s;

}

std::string id(RTLIL::IdString internal_id, bool may_rename = true)
{
	const char *str = internal_id.c_str();
	bool do_escape = false;

	if (may_rename && auto_name_map.count(internal_id) != 0) {
		// Thierry 
		// Return a legal VHDL name
		//
		return stringf("%sRS_%0*d", auto_prefix.c_str(), auto_name_digits, 
				auto_name_offset + auto_name_map[internal_id]);
	}

	if (*str == '\\')
		str++;

	if ('0' <= *str && *str <= '9')
		do_escape = true;

	for (int i = 0; str[i]; i++)
	{
		if ('0' <= str[i] && str[i] <= '9')
			continue;
		if ('a' <= str[i] && str[i] <= 'z')
			continue;
		if ('A' <= str[i] && str[i] <= 'Z')
			continue;
		if (str[i] == '_')
			continue;
		do_escape = true;
		break;
	}

	const pool<string> keywords = {
		// IEEE 1800-2017 Annex B
		"accept_on", "alias", "always", "always_comb", "always_ff", "always_latch", "and", "assert", "assign", "assume", "automatic", "before",
		"begin", "bind", "bins", "binsof", "bit", "break", "buf", "bufif0", "bufif1", "byte", "case", "casex", "casez", "cell", "chandle",
		"checker", "class", "clocking", "cmos", "config", "const", "constraint", "context", "continue", "cover", "covergroup", "coverpoint",
		"cross", "deassign", "default", "defparam", "design", "disable", "dist", "do", "edge", "else", "end", "endcase", "endchecker",
		"endclass", "endclocking", "endconfig", "endfunction", "endgenerate", "endgroup", "endinterface", "endmodule", "endpackage",
		"endprimitive", "endprogram", "endproperty", "endsequence", "endspecify", "endtable", "endtask", "enum", "event", "eventually",
		"expect", "export", "extends", "extern", "final", "first_match", "for", "force", "foreach", "forever", "fork", "forkjoin", "function",
		"generate", "genvar", "global", "highz0", "highz1", "if", "iff", "ifnone", "ignore_bins", "illegal_bins", "implements", "implies",
		"import", "incdir", "include", "initial", "inout", "input", "inside", "instance", "int", "integer", "interconnect", "interface",
		"intersect", "join", "join_any", "join_none", "large", "let", "liblist", "library", "local", "localparam", "logic", "longint",
		"macromodule", "matches", "medium", "modport", "module", "nand", "negedge", "nettype", "new", "nexttime", "nmos", "nor",
		"noshowcancelled", "not", "notif0", "notif1", "null", "or", "output", "package", "packed", "parameter", "pmos", "posedge", "primitive",
		"priority", "program", "property", "protected", "pull0", "pull1", "pulldown", "pullup", "pulsestyle_ondetect", "pulsestyle_onevent",
		"pure", "rand", "randc", "randcase", "randsequence", "rcmos", "real", "realtime", "ref", "reg", "reject_on", "release", "repeat",
		"restrict", "return", "rnmos", "rpmos", "rtran", "rtranif0", "rtranif1", "s_always", "s_eventually", "s_nexttime", "s_until",
		"s_until_with", "scalared", "sequence", "shortint", "shortreal", "showcancelled", "signed", "small", "soft", "solve", "specify",
		"specparam", "static", "string", "strong", "strong0", "strong1", "struct", "super", "supply0", "supply1", "sync_accept_on",
		"sync_reject_on", "table", "tagged", "task", "this", "throughout", "time", "timeprecision", "timeunit", "tran", "tranif0", "tranif1",
		"tri", "tri0", "tri1", "triand", "trior", "trireg", "type", "typedef", "union", "unique", "unique0", "unsigned", "until", "until_with",
		"untyped", "use", "uwire", "var", "vectored", "virtual", "void", "wait", "wait_order", "wand", "weak", "weak0", "weak1", "while",
		"wildcard", "wire", "with", "within", "wor", "xnor", "xor",
	};
	if (keywords.count(str))
		do_escape = true;

        return (legalizeVhdlName(str));

	if (do_escape)
		return "\\" + std::string(str) + " ";
	return std::string(str);
}

bool is_reg_wire(RTLIL::SigSpec sig, std::string &reg_name)
{
	if (!sig.is_chunk() || sig.as_chunk().wire == NULL)
		return false;

	RTLIL::SigChunk chunk = sig.as_chunk();

	if (reg_wires.count(chunk.wire->name) == 0)
		return false;

	reg_name = id(chunk.wire->name);
	if (sig.size() != chunk.wire->width) {
		if (sig.size() == 1)
			reg_name += stringf("[%d]", chunk.wire->start_offset +  chunk.offset);
		else if (chunk.wire->upto)
			reg_name += stringf("[%d:%d]", (chunk.wire->width - (chunk.offset + chunk.width - 1) - 1) + chunk.wire->start_offset,
					(chunk.wire->width - chunk.offset - 1) + chunk.wire->start_offset);
		else
			reg_name += stringf("[%d:%d]", chunk.wire->start_offset +  chunk.offset + chunk.width - 1,
					chunk.wire->start_offset +  chunk.offset);
	}

	return true;
}

void vhdl_dump_const(std::ostream &f, const RTLIL::Const &data, int width = -1, int offset = 0, bool no_decimal = false, bool escape_comment = false)
{
	bool set_signed = (data.flags & RTLIL::CONST_FLAG_SIGNED) != 0;
	if (width < 0)
		width = data.bits.size() - offset;
	if (width == 0) {
		// See IEEE 1364-2005 Clause 5.1.14.
		f << "{0{1'b0}}";
		return;
	}
	if (nostr)
		goto vhdl_dump_hex;
	if ((data.flags & RTLIL::CONST_FLAG_STRING) == 0 || width != (int)data.bits.size()) {
		if (width == 32 && !no_decimal && !nodec) {
			int32_t val = 0;
			for (int i = offset+width-1; i >= offset; i--) {
				log_assert(i < (int)data.bits.size());
				if (data.bits[i] != State::S0 && data.bits[i] != State::S1)
					goto vhdl_dump_hex;
				if (data.bits[i] == State::S1)
					val |= 1 << (i - offset);
			}
			if (decimal)
				f << stringf("%d", val);
			else if (set_signed && val < 0)
				f << stringf("-32'sd%u", -val);
			else
				f << stringf("32'%sd%u", set_signed ? "s" : "", val);
		} else {
	vhdl_dump_hex:
			if (nohex) {
                           goto vhdl_dump_bin;
			}

			vector<char> bin_digits, hex_digits;
			for (int i = offset; i < offset+width; i++) {
				log_assert(i < (int)data.bits.size());
				switch (data.bits[i]) {
				case State::S0: bin_digits.push_back('0'); break;
				case State::S1: bin_digits.push_back('1'); break;
				case RTLIL::Sx: bin_digits.push_back('x'); break;
				case RTLIL::Sz: bin_digits.push_back('z'); break;
				case RTLIL::Sa: bin_digits.push_back('?'); break;
				case RTLIL::Sm: log_error("Found marker state in final netlist.");
				}
			}
			if (GetSize(bin_digits) == 0)
				goto vhdl_dump_bin;
			while (GetSize(bin_digits) % 4 != 0)
				if (bin_digits.back() == '1')
					bin_digits.push_back('0');
				else
					bin_digits.push_back(bin_digits.back());
			for (int i = 0; i < GetSize(bin_digits); i += 4)
			{
				char bit_3 = bin_digits[i+3];
				char bit_2 = bin_digits[i+2];
				char bit_1 = bin_digits[i+1];
				char bit_0 = bin_digits[i+0];
				if (bit_3 == 'x' || bit_2 == 'x' || bit_1 == 'x' || bit_0 == 'x') {
					if (bit_3 != 'x' || bit_2 != 'x' || bit_1 != 'x' || bit_0 != 'x')
						goto vhdl_dump_bin;
					hex_digits.push_back('x');
					continue;
				}
				if (bit_3 == 'z' || bit_2 == 'z' || bit_1 == 'z' || bit_0 == 'z') {
					if (bit_3 != 'z' || bit_2 != 'z' || bit_1 != 'z' || bit_0 != 'z')
						goto vhdl_dump_bin;
					hex_digits.push_back('z');
					continue;
				}
				if (bit_3 == '?' || bit_2 == '?' || bit_1 == '?' || bit_0 == '?') {
					if (bit_3 != '?' || bit_2 != '?' || bit_1 != '?' || bit_0 != '?')
						goto vhdl_dump_bin;
					hex_digits.push_back('?');
					continue;
				}
				int val = 8*(bit_3 - '0') + 4*(bit_2 - '0') + 2*(bit_1 - '0') + (bit_0 - '0');
				hex_digits.push_back(val < 10 ? '0' + val : 'a' + val - 10);
			}
			f << stringf("%d'%sh", width, set_signed ? "s" : "");
			for (int i = GetSize(hex_digits)-1; i >= 0; i--)
				f << hex_digits[i];
		}
		if (0) {

	vhdl_dump_bin:

			//f << stringf("%d'%sb", width, set_signed ? "s" : "");
			// Thierry : creates some kind of VHDL parsing error if there is a sign
			if (set_signed) {
			}
			if (width == 0)
				f << stringf("0");
			if (width == 1)
			  f << stringf("\'");
			else
			  f << stringf("\"");

			for (int i = offset+width-1; i >= offset; i--) {
				log_assert(i < (int)data.bits.size());
				switch (data.bits[i]) {
				case State::S0: f << stringf("0"); break;
				case State::S1: f << stringf("1"); break;
				case RTLIL::Sx: f << stringf("X"); break;
				case RTLIL::Sz: f << stringf("Z"); break;
				case RTLIL::Sa: f << stringf("?"); break;
				case RTLIL::Sm: log_error("Found marker state in final netlist.");
				}
			}
			if (width == 1)
			  f << stringf("\'");
			else
			  f << stringf("\"");
		}
	} else {
		if ((data.flags & RTLIL::CONST_FLAG_REAL) == 0)
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
			else if (str[i] == '/' && escape_comment && i > 0 && str[i-1] == '*')
				f << stringf("\\/");
			else
				f << str[i];
		}
		if ((data.flags & RTLIL::CONST_FLAG_REAL) == 0)
			f << stringf("\"");
	}
}

void vhdl_dump_reg_init(std::ostream &f, SigSpec sig)
{
	Const initval;
	bool gotinit = false;

	for (auto bit : active_sigmap(sig)) {
		if (active_initdata.count(bit)) {
			initval.bits.push_back(active_initdata.at(bit));
			gotinit = true;
		} else {
			initval.bits.push_back(State::Sx);
		}
	}

	if (gotinit) {
		f << " = ";
		vhdl_dump_const(f, initval);
	}
}

void vhdl_dump_sigchunk(std::ostream &f, const RTLIL::SigChunk &chunk, bool no_decimal = false)
{
	if (chunk.wire == NULL) {
		vhdl_dump_const(f, chunk.data, chunk.width, chunk.offset, no_decimal);
	} else {
		if (chunk.width == chunk.wire->width && chunk.offset == 0) {
			f << stringf("%s", id(chunk.wire->name).c_str());
		} else if (chunk.width == 1) {
			if (chunk.wire->upto)
				f << stringf("%s(%d)", id(chunk.wire->name).c_str(), 
                                             (chunk.wire->width - chunk.offset - 1) + 
                                             chunk.wire->start_offset);
			else
				f << stringf("%s(%d)", id(chunk.wire->name).c_str(), 
                                             chunk.offset + chunk.wire->start_offset);
		} else {
			if (chunk.wire->upto)
				f << stringf("%s(%d upto %d)", id(chunk.wire->name).c_str(),
					     (chunk.wire->width - 
                                             (chunk.offset + chunk.width - 1) - 1) + 
                                             chunk.wire->start_offset, (chunk.wire->width - 
                                             chunk.offset - 1) + chunk.wire->start_offset);
			else
				f << stringf("%s(%d downto %d)", id(chunk.wire->name).c_str(),
					    (chunk.offset + chunk.width - 1) + chunk.wire->start_offset,
					    chunk.offset + chunk.wire->start_offset);
		}
	}
}

void vhdl_dump_sigspec(std::ostream &f, const RTLIL::SigSpec &sig, bool no_decimal = false)
	// toto
{
	if (GetSize(sig) == 0) {
		// See IEEE 1364-2005 Clause 5.1.14.
		f << "{0{1'b0}}";
		return;
	}
	if (sig.is_chunk()) {
		vhdl_dump_sigchunk(f, sig.as_chunk(), no_decimal);
	} else {
		f << stringf("( ");
		for (auto it = sig.chunks().rbegin(); it != sig.chunks().rend(); ++it) {
			if (it != sig.chunks().rbegin())
				f << stringf(" & ");
			vhdl_dump_sigchunk(f, *it, true);
		}
		f << stringf(")");
	}
}

void vhdl_dump_attributes(std::ostream &f, std::string indent, dict<RTLIL::IdString, RTLIL::Const> &attributes, char term = '\n', bool modattr = false, bool regattr = false, bool as_comment = false)
{
	if (noattr)
		return;
	if (attr2comment)
		as_comment = true;
	for (auto it = attributes.begin(); it != attributes.end(); ++it) {
		if (it->first == ID::init && regattr) continue;
		f << stringf("%s" "%s %s", indent.c_str(), as_comment ? "/*" : "(*", id(it->first).c_str());
		f << stringf(" = ");
		if (modattr && (it->second == State::S0 || it->second == Const(0)))
			f << stringf(" 0 ");
		else if (modattr && (it->second == State::S1 || it->second == Const(1)))
			f << stringf(" 1 ");
		else
			vhdl_dump_const(f, it->second, -1, 0, false, as_comment);
		f << stringf(" %s%c", as_comment ? "*/" : "*)", term);
	}
}

void vhdl_dump_signal(std::ostream &f, std::string indent, RTLIL::Wire *wire)
{
#if 1
	if (wire->port_input || wire->port_output) {
          return;
	}
#endif

	std::string range = "";
	if (wire->width != 1) {
		if (wire->upto)
			range = stringf("std_logic_vector(%d upto %d)", wire->start_offset, 
					wire->width - 1 + wire->start_offset);
		else
			range = stringf("std_logic_vector(%d downto %d)", 
					wire->width - 1 + wire->start_offset, wire->start_offset);
	} else {
		range = stringf("std_logic");
	}

	if (reg_wires.count(wire->name)) {
		f << stringf("%s" "reg%s %s", indent.c_str(), range.c_str(), id(wire->name).c_str());
		if (wire->attributes.count(ID::init)) {
			f << stringf(" = ");
			vhdl_dump_const(f, wire->attributes.at(ID::init));
		}
		f << stringf(";\n");
	} else {
		f << stringf("%s" "signal %s : %s ;\n", indent.c_str(), id(wire->name).c_str(), 
			     range.c_str());

	}
}

void vhdl_dump_memory(std::ostream &f, std::string indent, Mem &mem)
{
	std::string mem_id = id(mem.memid);

	vhdl_dump_attributes(f, indent, mem.attributes);
	f << stringf("%s" "reg [%d:0] %s [%d:%d];\n", indent.c_str(), mem.width-1, mem_id.c_str(), mem.size+mem.start_offset-1, mem.start_offset);

	// for memory block make something like:
	//  reg [7:0] memid [3:0];
	//  initial begin
	//    memid[0] = ...
	//  end
	if (!mem.inits.empty())
	{
		if (extmem)
		{
			std::string extmem_filename = stringf("%s-%d.mem", extmem_prefix.c_str(), extmem_counter++);

			std::string extmem_filename_esc;
			for (auto c : extmem_filename)
			{
				if (c == '\n')
					extmem_filename_esc += "\\n";
				else if (c == '\t')
					extmem_filename_esc += "\\t";
				else if (c < 32)
					extmem_filename_esc += stringf("\\%03o", c);
				else if (c == '"')
					extmem_filename_esc += "\\\"";
				else if (c == '\\')
					extmem_filename_esc += "\\\\";
				else
					extmem_filename_esc += c;
			}
			f << stringf("%s" "initial $readmemb(\"%s\", %s);\n", indent.c_str(), extmem_filename_esc.c_str(), mem_id.c_str());

			std::ofstream extmem_f(extmem_filename, std::ofstream::trunc);
			if (extmem_f.fail())
				log_error("Can't open file `%s' for writing: %s\n", extmem_filename.c_str(), strerror(errno));
			else
			{
				Const data = mem.get_init_data();
				for (int i=0; i<mem.size; i++)
				{
					RTLIL::Const element = data.extract(i*mem.width, mem.width);
					for (int j=0; j<element.size(); j++)
					{
						switch (element[element.size()-j-1])
						{
							case State::S0: extmem_f << '0'; break;
							case State::S1: extmem_f << '1'; break;
							case State::Sx: extmem_f << 'x'; break;
							case State::Sz: extmem_f << 'z'; break;
							case State::Sa: extmem_f << '_'; break;
							case State::Sm: log_error("Found marker state in final netlist.");
						}
					}
					extmem_f << '\n';
				}
			}
		}
		else
		{
			f << stringf("%s" "initial begin\n", indent.c_str());
			for (auto &init : mem.inits) {
				int words = GetSize(init.data) / mem.width;
				int start = init.addr.as_int();
				for (int i=0; i<words; i++)
				{
					for (int j = 0; j < mem.width; j++)
					{
						if (init.en[j] != State::S1)
							continue;

						int start_j = j, width = 1;

						while (j+1 < mem.width && init.en[j+1] == State::S1)
							j++, width++;

						if (width == mem.width) {
							f << stringf("%s" "  %s[%d] = ", indent.c_str(), mem_id.c_str(), i + start);
						} else {
							f << stringf("%s" "  %s[%d][%d:%d] = ", indent.c_str(), mem_id.c_str(), i + start, j, start_j);
						}
						vhdl_dump_const(f, init.data.extract(i*mem.width+start_j, width));
						f << stringf(";\n");
					}
				}
			}
			f << stringf("%s" "end\n", indent.c_str());
		}
	}

	// create a map : "edge clk" -> expressions within that clock domain
	dict<std::string, std::vector<std::string>> clk_to_lof_body;
	dict<std::string, std::string> clk_to_arst_cond;
	dict<std::string, std::vector<std::string>> clk_to_arst_body;
	clk_to_lof_body[""] = std::vector<std::string>();
	std::string clk_domain_str;
	// create a list of reg declarations
	std::vector<std::string> lof_reg_declarations;

	// read ports
	for (auto &port : mem.rd_ports)
	{
		if (port.clk_enable)
		{
			{
				std::ostringstream os;
				vhdl_dump_sigspec(os, port.clk);
				clk_domain_str = stringf("%sedge %s", port.clk_polarity ? "pos" : "neg", os.str().c_str());
				if (port.arst != State::S0) {
					std::ostringstream os2;
					vhdl_dump_sigspec(os2, port.arst);
					clk_domain_str += stringf(", posedge %s", os2.str().c_str());
					clk_to_arst_cond[clk_domain_str] = os2.str();
				}
			}

			// Decide how to represent the transparency; same idea as Mem::extract_rdff.
			bool trans_use_addr = true;
			for (auto bit : port.transparency_mask)
				if (!bit)
					trans_use_addr = false;

			if (GetSize(mem.wr_ports) == 0)
				trans_use_addr = false;

			if (port.en != State::S1 || port.srst != State::S0 || port.arst != State::S0 || !port.init_value.is_fully_undef())
				trans_use_addr = false;

			if (!trans_use_addr)
			{
				// for clocked read ports make something like:
				//   reg [..] temp_id;
				//   always @(posedge clk)
				//      if (rd_en) temp_id <= array_reg[r_addr];
				//   assign r_data = temp_id;
				std::string temp_id = next_auto_id();
				lof_reg_declarations.push_back( stringf("reg [%d:0] %s;\n", port.data.size() - 1, temp_id.c_str()) );

				bool has_indent = false;

				if (port.arst != State::S0) {
					std::ostringstream os;
					os << stringf("%s <= ", temp_id.c_str());
					vhdl_dump_sigspec(os, port.arst_value);
					os << ";\n";
					clk_to_arst_body[clk_domain_str].push_back(os.str());
				}

				if (port.srst != State::S0 && !port.ce_over_srst) {
					std::ostringstream os;
					os << stringf("if (");
					vhdl_dump_sigspec(os, port.srst);
					os << stringf(")\n");
					clk_to_lof_body[clk_domain_str].push_back(os.str());
					std::ostringstream os2;
					os2 << stringf("%s" "%s <= ", indent.c_str(), temp_id.c_str());
					vhdl_dump_sigspec(os2, port.srst_value);
					os2 << ";\n";
					clk_to_lof_body[clk_domain_str].push_back(os2.str());
					std::ostringstream os3;
					if (port.en == State::S1) {
						os3 << "else begin\n";
					} else {
						os3 << "else if (";
						vhdl_dump_sigspec(os3, port.en);
						os3 << ") begin\n";
					}
					clk_to_lof_body[clk_domain_str].push_back(os3.str());
					has_indent = true;
				} else if (port.en != State::S1) {
					std::ostringstream os;
					os << stringf("if (");
					vhdl_dump_sigspec(os, port.en);
					os << stringf(") begin\n");
					clk_to_lof_body[clk_domain_str].push_back(os.str());
					has_indent = true;
				}

				for (int sub = 0; sub < (1 << port.wide_log2); sub++)
				{
					SigSpec addr = port.sub_addr(sub);
					std::ostringstream os;
					if (has_indent)
						os << indent;
					os << temp_id;
					if (port.wide_log2)
						os << stringf("[%d:%d]", (sub + 1) * mem.width - 1, sub * mem.width);
					os << stringf(" <= %s[", mem_id.c_str());
					vhdl_dump_sigspec(os, addr);
					os << stringf("];\n");
					clk_to_lof_body[clk_domain_str].push_back(os.str());
				}

				for (int i = 0; i < GetSize(mem.wr_ports); i++) {
					auto &wport = mem.wr_ports[i];
					if (!port.transparency_mask[i] && !port.collision_x_mask[i])
						continue;
					int min_wide_log2 = std::min(port.wide_log2, wport.wide_log2);
					int max_wide_log2 = std::max(port.wide_log2, wport.wide_log2);
					bool wide_write = wport.wide_log2 > port.wide_log2;
					for (int sub = 0; sub < (1 << max_wide_log2); sub += (1 << min_wide_log2)) {
						SigSpec raddr = port.addr;
						SigSpec waddr = wport.addr;
						if (wide_write)
							waddr = wport.sub_addr(sub);
						else
							raddr = port.sub_addr(sub);
						int pos = 0;
						int ewidth = mem.width << min_wide_log2;
						int wsub = wide_write ? sub : 0;
						int rsub = wide_write ? 0 : sub;
						while (pos < ewidth) {
							int epos = pos;
							while (epos < ewidth && wport.en[epos + wsub * mem.width] == wport.en[pos + wsub * mem.width])
								epos++;

							std::ostringstream os;
							if (has_indent)
								os << indent;
							os << "if (";
							vhdl_dump_sigspec(os, wport.en[pos + wsub * mem.width]);
							if (raddr != waddr) {
								os << " && ";
								vhdl_dump_sigspec(os, raddr);
								os << " == ";
								vhdl_dump_sigspec(os, waddr);
							}
							os << ")\n";
							clk_to_lof_body[clk_domain_str].push_back(os.str());

							std::ostringstream os2;
							if (has_indent)
								os2 << indent;
							os2 << indent;
							os2 << temp_id;
							if (epos-pos != GetSize(port.data))
								os2 << stringf("[%d:%d]", rsub * mem.width + epos-1, rsub * mem.width + pos);
							os2 << " <= ";
							if (port.transparency_mask[i])
								vhdl_dump_sigspec(os2, wport.data.extract(wsub * mem.width + pos, epos-pos));
							else
								vhdl_dump_sigspec(os2, Const(State::Sx, epos - pos));
							os2 << ";\n";
							clk_to_lof_body[clk_domain_str].push_back(os2.str());

							pos = epos;
						}
					}
				}

				if (port.srst != State::S0 && port.ce_over_srst)
				{
					std::ostringstream os;
					if (has_indent)
						os << indent;
					os << stringf("if (");
					vhdl_dump_sigspec(os, port.srst);
					os << stringf(")\n");
					clk_to_lof_body[clk_domain_str].push_back(os.str());
					std::ostringstream os2;
					if (has_indent)
						os2 << indent;
					os2 << stringf("%s" "%s <= ", indent.c_str(), temp_id.c_str());
					vhdl_dump_sigspec(os2, port.srst_value);
					os2 << ";\n";
					clk_to_lof_body[clk_domain_str].push_back(os2.str());
				}

				if (has_indent)
					clk_to_lof_body[clk_domain_str].push_back("end\n");

				if (!port.init_value.is_fully_undef())
				{
					std::ostringstream os;
					vhdl_dump_sigspec(os, port.init_value);
					std::string line = stringf("initial %s = %s;\n", temp_id.c_str(), os.str().c_str());
					clk_to_lof_body[""].push_back(line);
				}

				{
					std::ostringstream os;
					vhdl_dump_sigspec(os, port.data);
					std::string line = stringf("1assign %s = %s;\n", os.str().c_str(), temp_id.c_str());
					clk_to_lof_body[""].push_back(line);
				}
			}
			else
			{
				// for rd-transparent read-ports make something like:
				//   reg [..] temp_id;
				//   always @(posedge clk)
				//     temp_id <= r_addr;
				//   assign r_data = array_reg[temp_id];
				std::string temp_id = next_auto_id();
				lof_reg_declarations.push_back( stringf("reg [%d:0] %s;\n", port.addr.size() - 1 - port.wide_log2, temp_id.c_str()) );
				{
					std::ostringstream os;
					vhdl_dump_sigspec(os, port.addr.extract_end(port.wide_log2));
					std::string line = stringf("%s <= %s;\n", temp_id.c_str(), os.str().c_str());
					clk_to_lof_body[clk_domain_str].push_back(line);
				}
				for (int sub = 0; sub < (1 << port.wide_log2); sub++)
				{
					std::ostringstream os;
					os << "2assign ";
					vhdl_dump_sigspec(os, port.data.extract(sub * mem.width, mem.width));
					os << stringf(" = %s[", mem_id.c_str());;
					if (port.wide_log2) {
						Const addr_lo;
						for (int i = 0; i < port.wide_log2; i++)
							addr_lo.bits.push_back(State(sub >> i & 1));
						os << "{";
						os << temp_id;
						os << ", ";
						vhdl_dump_const(os, addr_lo);
						os << "}";
					} else {
						os << temp_id;
					}
					os << "];\n";
					clk_to_lof_body[""].push_back(os.str());
				}
			}
		} else {
			// for non-clocked read-ports make something like:
			//   assign r_data = array_reg[r_addr];
			for (int sub = 0; sub < (1 << port.wide_log2); sub++)
			{
				SigSpec addr = port.sub_addr(sub);

				std::ostringstream os, os2;
				vhdl_dump_sigspec(os, port.data.extract(sub * mem.width, mem.width));
				vhdl_dump_sigspec(os2, addr);
				std::string line = stringf("3assign %s = %s[%s];\n", os.str().c_str(), mem_id.c_str(), os2.str().c_str());
				clk_to_lof_body[""].push_back(line);
			}
		}
	}

	// Write ports.  Those are messy because we try to preserve priority, as much as we can:
	//
	// 1. We split all ports into several disjoint processes.
	// 2. If a port has priority over another port, the two ports need to share
	//    a process, so that priority can be reconstructed on the other end.
	// 3. We want each process to be as small as possible, to avoid extra
	//    priorities inferred on the other end.
	pool<int> wr_ports_done;
	for (int ridx = 0; ridx < GetSize(mem.wr_ports); ridx++)
	{
		if (wr_ports_done.count(ridx))
			continue;

		auto &root = mem.wr_ports[ridx];

		// Start from a root.
		pool<int> wr_ports_now;
		wr_ports_now.insert(ridx);

		// Transitively fill list of ports in this process by following priority edges.
		while (true)
		{
			bool changed = false;

			for (int i = 0; i < GetSize(mem.wr_ports); i++)
			for (int j = 0; j < i; j++)
			if (mem.wr_ports[i].priority_mask[j])
			{
				if (wr_ports_now.count(i) && !wr_ports_now.count(j)) {
					wr_ports_now.insert(j);
					changed = true;
				}
				if (!wr_ports_now.count(i) && wr_ports_now.count(j)) {
					wr_ports_now.insert(i);
					changed = true;
				}
			}

			if (!changed)
				break;
		}

		if (root.clk_enable) {
			f << stringf("%s" "always%s @(%sedge ", indent.c_str(), systemvhdl ? "_ff" : "", root.clk_polarity ? "pos" : "neg");
			vhdl_dump_sigspec(f, root.clk);
			f << ") begin\n";
		} else {
			f << stringf("%s" "always%s begin\n", indent.c_str(), systemvhdl ? "_latch" : " @*");
		}

		for (int pidx = 0; pidx < GetSize(mem.wr_ports); pidx++)
		{
			if (!wr_ports_now.count(pidx))
				continue;
			wr_ports_done.insert(pidx);

			auto &port = mem.wr_ports[pidx];
			log_assert(port.clk_enable == root.clk_enable);
			if (port.clk_enable) {
				log_assert(port.clk == root.clk);
				log_assert(port.clk_polarity == root.clk_polarity);
			}

			//   make something like:
			//   always @(posedge clk)
			//      if (wr_en_bit) memid[w_addr][??] <= w_data[??];
			//   ...
			for (int sub = 0; sub < (1 << port.wide_log2); sub++)
			{
				SigSpec addr = port.sub_addr(sub);
				for (int i = 0; i < mem.width; i++)
				{
					int start_i = i, width = 1;
					SigBit wen_bit = port.en[sub * mem.width + i];

					while (i+1 < mem.width && active_sigmap(port.en[sub * mem.width + i+1]) == active_sigmap(wen_bit))
						i++, width++;

					if (wen_bit == State::S0)
						continue;

					f << stringf("%s%s", indent.c_str(), indent.c_str());
					if (wen_bit != State::S1)
					{
						f << stringf("if (");
						vhdl_dump_sigspec(f, wen_bit);
						f << stringf(")\n");
						f << stringf("%s%s%s", indent.c_str(), indent.c_str(), indent.c_str());
					}
					f << stringf("%s[", mem_id.c_str());
					vhdl_dump_sigspec(f, addr);
					if (width == GetSize(port.en))
						f << stringf("] <= ");
					else
						f << stringf("][%d:%d] <= ", i, start_i);
					vhdl_dump_sigspec(f, port.data.extract(sub * mem.width + start_i, width));
					f << stringf(";\n");
				}
			}
		}

		f << stringf("%s" "end\n", indent.c_str());
	}
	// Output Vhdl that looks something like this:
	// reg [..] _3_;
	// always @(posedge CLK2) begin
	//   _3_ <= memory[D1ADDR];
	//   if (A1EN)
	//     memory[A1ADDR] <= A1DATA;
	//   if (A2EN)
	//     memory[A2ADDR] <= A2DATA;
	//   ...
	// end
	// always @(negedge CLK1) begin
	//   if (C1EN)
	//     memory[C1ADDR] <= C1DATA;
	// end
	// ...
	// assign D1DATA = _3_;
	// assign D2DATA <= memory[D2ADDR];

	// the reg ... definitions
	for(auto &reg : lof_reg_declarations)
	{
		f << stringf("%s" "%s", indent.c_str(), reg.c_str());
	}
	// the block of expressions by clock domain
	for(auto &pair : clk_to_lof_body)
	{
		std::string clk_domain = pair.first;
		std::vector<std::string> lof_lines = pair.second;
		if( clk_domain != "")
		{
			f << stringf("%s" "always%s @(%s) begin\n", indent.c_str(), systemvhdl ? "_ff" : "", clk_domain.c_str());
			bool has_arst = clk_to_arst_cond.count(clk_domain) != 0;
			if (has_arst) {
				f << stringf("%s%s" "if (%s) begin\n", indent.c_str(), indent.c_str(), clk_to_arst_cond[clk_domain].c_str());
				for(auto &line : clk_to_arst_body[clk_domain])
					f << stringf("%s%s%s" "%s", indent.c_str(), indent.c_str(), indent.c_str(), line.c_str());
				f << stringf("%s%s" "end else begin\n", indent.c_str(), indent.c_str());
				for(auto &line : lof_lines)
					f << stringf("%s%s%s" "%s", indent.c_str(), indent.c_str(), indent.c_str(), line.c_str());
				f << stringf("%s%s" "end\n", indent.c_str(), indent.c_str());
			} else {
				for(auto &line : lof_lines)
					f << stringf("%s%s" "%s", indent.c_str(), indent.c_str(), line.c_str());
			}
			f << stringf("%s" "end\n", indent.c_str());
		}
		else
		{
			// the non-clocked assignments
			for(auto &line : lof_lines)
				f << stringf("%s" "%s", indent.c_str(), line.c_str());
		}
	}
}

void vhdl_dump_cell_expr_port(std::ostream &f, RTLIL::Cell *cell, std::string port, bool gen_signed = true)
{
	if (gen_signed && cell->parameters.count("\\" + port + "_SIGNED") > 0 && 
            cell->parameters["\\" + port + "_SIGNED"].as_bool()) {

          f << stringf("$signed(");
          vhdl_dump_sigspec(f, cell->getPort("\\" + port));
          f << stringf(")");

	} else {

          vhdl_dump_sigspec(f, cell->getPort("\\" + port));

	}
}

std::string cellname(RTLIL::Cell *cell)
{
	if (!norename && cell->name[0] == '$' && RTLIL::builtin_ff_cell_types().count(cell->type) && cell->hasPort(ID::Q) && !cell->type.in(ID($ff), ID($_FF_)))
	{
		RTLIL::SigSpec sig = cell->getPort(ID::Q);
		if (GetSize(sig) != 1 || sig.is_fully_const())
			goto no_special_reg_name;

		RTLIL::Wire *wire = sig[0].wire;

		if (wire->name[0] != '\\')
			goto no_special_reg_name;

		std::string cell_name = wire->name.str();

		size_t pos = cell_name.find('[');
		if (pos != std::string::npos)
			cell_name = cell_name.substr(0, pos) + "_reg" + cell_name.substr(pos);
		else
			cell_name = cell_name + "_reg";

		if (wire->width != 1)
			cell_name += stringf("[%d]", wire->start_offset + sig[0].offset);

		if (active_module && active_module->count_id(cell_name) > 0)
				goto no_special_reg_name;

		return id(cell_name);
	}
	else
	{
no_special_reg_name:
		return id(cell->name).c_str();
	}
}

void vhdl_dump_cell_expr_uniop(std::ostream &f, std::string indent, RTLIL::Cell *cell, std::string op)
{
	// Thierry
	//
        log_cmd_error("Error: write_vhdl cannot handle unary operator '%s'\n", op.c_str());

	f << stringf("%s" "4assign ", indent.c_str());
	vhdl_dump_sigspec(f, cell->getPort(ID::Y));
	f << stringf(" = %s ", op.c_str());
	vhdl_dump_attributes(f, "", cell->attributes, ' ');
	vhdl_dump_cell_expr_port(f, cell, "A", true);
	f << stringf(";\n");
}

void vhdl_dump_cell_expr_binop(std::ostream &f, std::string indent, RTLIL::Cell *cell, std::string op)
{
	// Thierry : support only ">>" binary operator
	//
        log_cmd_error("Error: write_vhdl cannot handle binary operator '%s'\n", op.c_str());

	string cellName = id(cell->type, false);

	log_cmd_error("Cell name is : %s\n", cellName.c_str());

	// No decimal for first argument of '>>'
	//
	vhdl_dump_sigspec(f, cell->getPort(ID::Y), true /* no_decimal !!! */);

	f << stringf("%s => ", indent.c_str());

	vhdl_dump_cell_expr_port(f, cell, "A", true);

	f << stringf(" %s ", op.c_str());

	vhdl_dump_attributes(f, "", cell->attributes, ' ');

	vhdl_dump_cell_expr_port(f, cell, "B", true);

	f << stringf(";\n");
}

bool vhdl_dump_cell_expr(std::ostream &f, std::string indent, RTLIL::Cell *cell)
{
	if (cell->type == ID($_NOT_)) {
		f << stringf("%s" "5assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		f << stringf("~");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(";\n");
		return true;
	}

	if (cell->type.in(ID($_AND_), ID($_NAND_), ID($_OR_), ID($_NOR_), ID($_XOR_), ID($_XNOR_), ID($_ANDNOT_), ID($_ORNOT_))) {
		f << stringf("%s" "6assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		if (cell->type.in(ID($_NAND_), ID($_NOR_), ID($_XNOR_)))
			f << stringf("~(");
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(" ");
		if (cell->type.in(ID($_AND_), ID($_NAND_), ID($_ANDNOT_)))
			f << stringf("&");
		if (cell->type.in(ID($_OR_), ID($_NOR_), ID($_ORNOT_)))
			f << stringf("|");
		if (cell->type.in(ID($_XOR_), ID($_XNOR_)))
			f << stringf("^");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		f << stringf(" ");
		if (cell->type.in(ID($_ANDNOT_), ID($_ORNOT_)))
			f << stringf("~(");
		vhdl_dump_cell_expr_port(f, cell, "B", false);
		if (cell->type.in(ID($_NAND_), ID($_NOR_), ID($_XNOR_), ID($_ANDNOT_), ID($_ORNOT_)))
			f << stringf(")");
		f << stringf(";\n");
		return true;
	}

	if (cell->type == ID($_MUX_)) {
		f << stringf("%s" "7assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		vhdl_dump_cell_expr_port(f, cell, "S", false);
		f << stringf(" ? ");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		vhdl_dump_cell_expr_port(f, cell, "B", false);
		f << stringf(" : ");
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(";\n");
		return true;
	}

	if (cell->type == ID($_NMUX_)) {
		f << stringf("%s" "8assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = !(");
		vhdl_dump_cell_expr_port(f, cell, "S", false);
		f << stringf(" ? ");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		vhdl_dump_cell_expr_port(f, cell, "B", false);
		f << stringf(" : ");
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(");\n");
		return true;
	}

	if (cell->type.in(ID($_AOI3_), ID($_OAI3_))) {
		f << stringf("%s" "9assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ~((");
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(cell->type == ID($_AOI3_) ? " & " : " | ");
		vhdl_dump_cell_expr_port(f, cell, "B", false);
		f << stringf(cell->type == ID($_AOI3_) ? ") |" : ") &");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		f << stringf(" ");
		vhdl_dump_cell_expr_port(f, cell, "C", false);
		f << stringf(");\n");
		return true;
	}

	if (cell->type.in(ID($_AOI4_), ID($_OAI4_))) {
		f << stringf("%s" "10assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ~((");
		vhdl_dump_cell_expr_port(f, cell, "A", false);
		f << stringf(cell->type == ID($_AOI4_) ? " & " : " | ");
		vhdl_dump_cell_expr_port(f, cell, "B", false);
		f << stringf(cell->type == ID($_AOI4_) ? ") |" : ") &");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		f << stringf(" (");
		vhdl_dump_cell_expr_port(f, cell, "C", false);
		f << stringf(cell->type == ID($_AOI4_) ? " & " : " | ");
		vhdl_dump_cell_expr_port(f, cell, "D", false);
		f << stringf("));\n");
		return true;
	}

#define HANDLE_UNIOP(_type, _operator) \
	if (cell->type ==_type) { vhdl_dump_cell_expr_uniop(f, indent, cell, _operator); return true; }
#define HANDLE_BINOP(_type, _operator) \
	if (cell->type ==_type) { vhdl_dump_cell_expr_binop(f, indent, cell, _operator); return true; }

	HANDLE_UNIOP(ID($not), "~")
	HANDLE_UNIOP(ID($pos), "+")
	HANDLE_UNIOP(ID($neg), "-")

	HANDLE_BINOP(ID($and),  "&")
	HANDLE_BINOP(ID($or),   "|")
	HANDLE_BINOP(ID($xor),  "^")
	HANDLE_BINOP(ID($xnor), "~^")

	HANDLE_UNIOP(ID($reduce_and),  "&")
	HANDLE_UNIOP(ID($reduce_or),   "|")
	HANDLE_UNIOP(ID($reduce_xor),  "^")
	HANDLE_UNIOP(ID($reduce_xnor), "~^")
	HANDLE_UNIOP(ID($reduce_bool), "|")

	HANDLE_BINOP(ID($shl),  "<<")
	HANDLE_BINOP(ID($shr),  ">>")
	HANDLE_BINOP(ID($sshl), "<<<")
	HANDLE_BINOP(ID($sshr), ">>>")

	HANDLE_BINOP(ID($lt),  "<")
	HANDLE_BINOP(ID($le),  "<=")
	HANDLE_BINOP(ID($eq),  "==")
	HANDLE_BINOP(ID($ne),  "!=")
	HANDLE_BINOP(ID($eqx), "===")
	HANDLE_BINOP(ID($nex), "!==")
	HANDLE_BINOP(ID($ge),  ">=")
	HANDLE_BINOP(ID($gt),  ">")

	HANDLE_BINOP(ID($add), "+")
	HANDLE_BINOP(ID($sub), "-")
	HANDLE_BINOP(ID($mul), "*")
	HANDLE_BINOP(ID($div), "/")
	HANDLE_BINOP(ID($mod), "%")
	HANDLE_BINOP(ID($pow), "**")

	HANDLE_UNIOP(ID($logic_not), "!")
	HANDLE_BINOP(ID($logic_and), "&&")
	HANDLE_BINOP(ID($logic_or),  "||")

#undef HANDLE_UNIOP
#undef HANDLE_BINOP

	if (cell->type == ID($divfloor))
	{
		// wire [MAXLEN+1:0] _0_, _1_, _2_;
		// assign _0_ = $signed(A);
		// assign _1_ = $signed(B);
		// assign _2_ = (A[-1] == B[-1]) || A == 0 ? _0_ : $signed(_0_ - (B[-1] ? _1_ + 1 : _1_ - 1));
		// assign Y = $signed(_2_) / $signed(_1_);

		if (cell->getParam(ID::A_SIGNED).as_bool() && cell->getParam(ID::B_SIGNED).as_bool()) {
			SigSpec sig_a = cell->getPort(ID::A);
			SigSpec sig_b = cell->getPort(ID::B);

			std::string buf_a = next_auto_id();
			std::string buf_b = next_auto_id();
			std::string buf_num = next_auto_id();
			int size_a = GetSize(sig_a);
			int size_b = GetSize(sig_b);
			int size_y = GetSize(cell->getPort(ID::Y));
			int size_max = std::max(size_a, std::max(size_b, size_y));

			// intentionally one wider than maximum width
			f << stringf("%s" "wire [%d:0] %s, %s, %s;\n", indent.c_str(), size_max, buf_a.c_str(), buf_b.c_str(), buf_num.c_str());
			f << stringf("%s" "11assign %s = ", indent.c_str(), buf_a.c_str());
			vhdl_dump_cell_expr_port(f, cell, "A", true);
			f << stringf(";\n");
			f << stringf("%s" "12assign %s = ", indent.c_str(), buf_b.c_str());
			vhdl_dump_cell_expr_port(f, cell, "B", true);
			f << stringf(";\n");

			f << stringf("%s" "13assign %s = ", indent.c_str(), buf_num.c_str());
			f << stringf("(");
			vhdl_dump_sigspec(f, sig_a.extract(sig_a.size()-1));
			f << stringf(" == ");
			vhdl_dump_sigspec(f, sig_b.extract(sig_b.size()-1));
			f << stringf(") || ");
			vhdl_dump_sigspec(f, sig_a);
			f << stringf(" == 0 ? %s : ", buf_a.c_str());
			f << stringf("$signed(%s - (", buf_a.c_str());
			vhdl_dump_sigspec(f, sig_b.extract(sig_b.size()-1));
			f << stringf(" ? %s + 1 : %s - 1));\n", buf_b.c_str(), buf_b.c_str());


			f << stringf("%s" "14assign ", indent.c_str());
			vhdl_dump_sigspec(f, cell->getPort(ID::Y));
			f << stringf(" = $signed(%s) / ", buf_num.c_str());
			vhdl_dump_attributes(f, "", cell->attributes, ' ');
			f << stringf("$signed(%s);\n", buf_b.c_str());
			return true;
		} else {
			// same as truncating division
			vhdl_dump_cell_expr_binop(f, indent, cell, "/");
			return true;
		}
	}

	if (cell->type == ID($modfloor))
	{
		// wire truncated = $signed(A) % $signed(B);
		// assign Y = (A[-1] == B[-1]) || truncated == 0 ? truncated : $signed(B) + $signed(truncated);

		if (cell->getParam(ID::A_SIGNED).as_bool() && cell->getParam(ID::B_SIGNED).as_bool()) {
			SigSpec sig_a = cell->getPort(ID::A);
			SigSpec sig_b = cell->getPort(ID::B);

			std::string temp_id = next_auto_id();
			f << stringf("%s" "wire [%d:0] %s = ", indent.c_str(), GetSize(cell->getPort(ID::A))-1, temp_id.c_str());
			vhdl_dump_cell_expr_port(f, cell, "A", true);
			f << stringf(" %% ");
			vhdl_dump_attributes(f, "", cell->attributes, ' ');
			vhdl_dump_cell_expr_port(f, cell, "B", true);
			f << stringf(";\n");

			f << stringf("%s" "15assign ", indent.c_str());
			vhdl_dump_sigspec(f, cell->getPort(ID::Y));
			f << stringf(" = (");
			vhdl_dump_sigspec(f, sig_a.extract(sig_a.size()-1));
			f << stringf(" == ");
			vhdl_dump_sigspec(f, sig_b.extract(sig_b.size()-1));
			f << stringf(") || %s == 0 ? %s : ", temp_id.c_str(), temp_id.c_str());
			vhdl_dump_cell_expr_port(f, cell, "B", true);
			f << stringf(" + $signed(%s);\n", temp_id.c_str());
			return true;
		} else {
			// same as truncating modulo
			vhdl_dump_cell_expr_binop(f, indent, cell, "%");
			return true;
		}
	}

	if (cell->type == ID($shift))
	{
		f << stringf("%s" "16assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		if (cell->getParam(ID::B_SIGNED).as_bool())
		{
			vhdl_dump_cell_expr_port(f, cell, "B", true);
			f << stringf(" < 0 ? ");
			vhdl_dump_cell_expr_port(f, cell, "A", true);
			f << stringf(" << - ");
			vhdl_dump_sigspec(f, cell->getPort(ID::B));
			f << stringf(" : ");
			vhdl_dump_cell_expr_port(f, cell, "A", true);
			f << stringf(" >> ");
			vhdl_dump_sigspec(f, cell->getPort(ID::B));
		}
		else
		{
			vhdl_dump_cell_expr_port(f, cell, "A", true);
			f << stringf(" >> ");
			vhdl_dump_sigspec(f, cell->getPort(ID::B));
		}
		f << stringf(";\n");
		return true;
	}

	if (cell->type == ID($shiftx))
	{
		std::string temp_id = next_auto_id();
		f << stringf("%s" "wire [%d:0] %s = ", indent.c_str(), GetSize(cell->getPort(ID::A))-1, temp_id.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(";\n");

		f << stringf("%s" "17assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = %s[", temp_id.c_str());
		if (cell->getParam(ID::B_SIGNED).as_bool())
			f << stringf("$signed(");
		vhdl_dump_sigspec(f, cell->getPort(ID::B));
		if (cell->getParam(ID::B_SIGNED).as_bool())
			f << stringf(")");
		f << stringf(" +: %d", cell->getParam(ID::Y_WIDTH).as_int());
		f << stringf("];\n");
		return true;
	}

	if (cell->type == ID($mux))
	{
		f << stringf("%s" "18assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		vhdl_dump_sigspec(f, cell->getPort(ID::S));
		f << stringf(" ? ");
		vhdl_dump_attributes(f, "", cell->attributes, ' ');
		vhdl_dump_sigspec(f, cell->getPort(ID::B));
		f << stringf(" : ");
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(";\n");
		return true;
	}

	if (cell->type == ID($pmux))
	{
		int width = cell->parameters[ID::WIDTH].as_int();
		int s_width = cell->getPort(ID::S).size();
		std::string func_name = cellname(cell);

		f << stringf("%s" "function [%d:0] %s;\n", indent.c_str(), width-1, func_name.c_str());
		f << stringf("%s" "  input [%d:0] a;\n", indent.c_str(), width-1);
		f << stringf("%s" "  input [%d:0] b;\n", indent.c_str(), s_width*width-1);
		f << stringf("%s" "  input [%d:0] s;\n", indent.c_str(), s_width-1);

		vhdl_dump_attributes(f, indent + "  ", cell->attributes);
		if (!noattr)
			f << stringf("%s" "  (* parallel_case *)\n", indent.c_str());
		f << stringf("%s" "  casez (s)", indent.c_str());
		f << stringf(noattr ? " // synopsys parallel_case\n" : "\n");

		for (int i = 0; i < s_width; i++)
		{
			f << stringf("%s" "    %d'b", indent.c_str(), s_width);

			for (int j = s_width-1; j >= 0; j--)
				f << stringf("%c", j == i ? '1' : '?');

			f << stringf(":\n");
			f << stringf("%s" "      %s = b[%d:%d];\n", indent.c_str(), func_name.c_str(), (i+1)*width-1, i*width);
		}

		f << stringf("%s" "    default:\n", indent.c_str());
		f << stringf("%s" "      %s = a;\n", indent.c_str(), func_name.c_str());

		f << stringf("%s" "  endcase\n", indent.c_str());
		f << stringf("%s" "endfunction\n", indent.c_str());

		f << stringf("%s" "19assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = %s(", func_name.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(", ");
		vhdl_dump_sigspec(f, cell->getPort(ID::B));
		f << stringf(", ");
		vhdl_dump_sigspec(f, cell->getPort(ID::S));
		f << stringf(");\n");
		return true;
	}

	if (cell->type == ID($tribuf))
	{
		f << stringf("%s" "20assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		vhdl_dump_sigspec(f, cell->getPort(ID::EN));
		f << stringf(" ? ");
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(" : %d'bz;\n", cell->parameters.at(ID::WIDTH).as_int());
		return true;
	}

	if (cell->type == ID($slice))
	{
		f << stringf("%s" "21assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = ");
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(" >> %d;\n", cell->parameters.at(ID::OFFSET).as_int());
		return true;
	}

	if (cell->type == ID($concat))
	{
		f << stringf("%s" "22assign ", indent.c_str());
		vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf(" = { ");
		vhdl_dump_sigspec(f, cell->getPort(ID::B));
		f << stringf(" , ");
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(" };\n");
		return true;
	}

	if (cell->type == ID($lut))
	{
		//f << stringf("%s" "23assign ", indent.c_str());
	        const RTLIL::Const data = cell->parameters.at(ID::LUT);
		const RTLIL::SigSpec sig = cell->getPort(ID::A);

		int widthData = data.bits.size();
		int widthSig = sig.size();

	        f << stringf("%s" "%s : lut \n", indent.c_str(), cellname(cell).c_str());
		f << stringf("     generic map (\n");
		f << stringf("        A_SIGNED => 0, \n");
		f << stringf("        B_SIGNED => 0, \n");
		f << stringf("        A_WIDTH => %d, \n", widthData);
		f << stringf("        B_WIDTH => %d, \n", widthSig);
		f << stringf("        Y_WIDTH => %d \n", widthData);
		f << stringf("      )\n");

		f << stringf("     port map (\n");

		// to handle std_logic to std_logic_vector conversion problem
		//
		f << stringf("        A => \"\" & ");

		vhdl_dump_const(f, cell->parameters.at(ID::LUT), -1, 0, true /* no decimal */, false);
		f << stringf(" ,\n");

		// to handle std_logic to std_logic_vector conversion problem
		//
		f << stringf("        B => \"\" & ");

		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(" ,\n");

		f << stringf("        Y => ");
	        vhdl_dump_sigspec(f, cell->getPort(ID::Y));
		f << stringf("\n");

		f << stringf("     );\n");

		return true;
	}

	if (RTLIL::builtin_ff_cell_types().count(cell->type))
	{
		FfData ff(nullptr, cell);

		// $ff / $_FF_ cell: not supported.
		if (ff.has_gclk)
			return false;

		std::string reg_name = cellname(cell);
		bool out_is_reg_wire = is_reg_wire(ff.sig_q, reg_name);

		if (!out_is_reg_wire) {
			if (ff.width == 1)
				f << stringf("%s" "reg %s", indent.c_str(), reg_name.c_str());
			else
				f << stringf("%s" "reg [%d:0] %s", indent.c_str(), ff.width-1, reg_name.c_str());
			vhdl_dump_reg_init(f, ff.sig_q);
			f << ";\n";
		}

		// If the FF has CLR/SET inputs, emit every bit slice separately.
		int chunks = ff.has_sr ? ff.width : 1;
		bool chunky = ff.has_sr && ff.width != 1;

		for (int i = 0; i < chunks; i++)
		{
			SigSpec sig_d, sig_ad;
			Const val_arst, val_srst;
			std::string reg_bit_name, sig_set_name, sig_clr_name, sig_arst_name, sig_aload_name;
			if (chunky) {
				reg_bit_name = stringf("%s[%d]", reg_name.c_str(), i);
				if (ff.has_gclk || ff.has_clk)
					sig_d = ff.sig_d[i];
				if (ff.has_aload)
					sig_ad = ff.sig_ad[i];
			} else {
				reg_bit_name = reg_name;
				sig_d = ff.sig_d;
				sig_ad = ff.sig_ad;
			}
			if (ff.has_arst)
				val_arst = chunky ? ff.val_arst[i] : ff.val_arst;
			if (ff.has_srst)
				val_srst = chunky ? ff.val_srst[i] : ff.val_srst;

			// If there are constants in the sensitivity list, replace them with an intermediate wire
			if (ff.has_clk) {
				if (ff.has_sr) {
					if (ff.sig_set[i].wire == NULL)
					{
						sig_set_name = next_auto_id();
						f << stringf("%s" "wire %s = ", indent.c_str(), sig_set_name.c_str());
						vhdl_dump_const(f, ff.sig_set[i].data);
						f << stringf(";\n");
					}
					if (ff.sig_clr[i].wire == NULL)
					{
						sig_clr_name = next_auto_id();
						f << stringf("%s" "wire %s = ", indent.c_str(), sig_clr_name.c_str());
						vhdl_dump_const(f, ff.sig_clr[i].data);
						f << stringf(";\n");
					}
				} else if (ff.has_arst) {
					if (ff.sig_arst[0].wire == NULL)
					{
						sig_arst_name = next_auto_id();
						f << stringf("%s" "wire %s = ", indent.c_str(), sig_arst_name.c_str());
						vhdl_dump_const(f, ff.sig_arst[0].data);
						f << stringf(";\n");
					}
				} else if (ff.has_aload) {
					if (ff.sig_aload[0].wire == NULL)
					{
						sig_aload_name = next_auto_id();
						f << stringf("%s" "wire %s = ", indent.c_str(), sig_aload_name.c_str());
						vhdl_dump_const(f, ff.sig_aload[0].data);
						f << stringf(";\n");
					}
				}
			}

			vhdl_dump_attributes(f, indent, cell->attributes);
			if (ff.has_clk)
			{
				// FFs.
				f << stringf("%s" "always%s @(%sedge ", indent.c_str(), systemvhdl ? "_ff" : "", ff.pol_clk ? "pos" : "neg");
				vhdl_dump_sigspec(f, ff.sig_clk);
				if (ff.has_sr) {
					f << stringf(", %sedge ", ff.pol_set ? "pos" : "neg");
					if (ff.sig_set[i].wire == NULL)
						f << stringf("%s", sig_set_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_set[i]);

					f << stringf(", %sedge ", ff.pol_clr ? "pos" : "neg");
					if (ff.sig_clr[i].wire == NULL)
						f << stringf("%s", sig_clr_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_clr[i]);
				} else if (ff.has_arst) {
					f << stringf(", %sedge ", ff.pol_arst ? "pos" : "neg");
					if (ff.sig_arst[0].wire == NULL)
						f << stringf("%s", sig_arst_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_arst);
				} else if (ff.has_aload) {
					f << stringf(", %sedge ", ff.pol_aload ? "pos" : "neg");
					if (ff.sig_aload[0].wire == NULL)
						f << stringf("%s", sig_aload_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_aload);
				}
				f << stringf(")\n");

				f << stringf("%s" "  ", indent.c_str());
				if (ff.has_sr) {
					f << stringf("if (%s", ff.pol_clr ? "" : "!");
					if (ff.sig_clr[i].wire == NULL)
						f << stringf("%s", sig_clr_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_clr[i]);
					f << stringf(") %s <= 1'b0;\n", reg_bit_name.c_str());
					f << stringf("%s" "  else if (%s", indent.c_str(), ff.pol_set ? "" : "!");
					if (ff.sig_set[i].wire == NULL)
						f << stringf("%s", sig_set_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_set[i]);
					f << stringf(") %s <= 1'b1;\n", reg_bit_name.c_str());
					f << stringf("%s" "  else ", indent.c_str());
				} else if (ff.has_arst) {
					f << stringf("if (%s", ff.pol_arst ? "" : "!");
					if (ff.sig_arst[0].wire == NULL)
						f << stringf("%s", sig_arst_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_arst);
					f << stringf(") %s <= ", reg_bit_name.c_str());
					vhdl_dump_sigspec(f, val_arst);
					f << stringf(";\n");
					f << stringf("%s" "  else ", indent.c_str());
				} else if (ff.has_aload) {
					f << stringf("if (%s", ff.pol_aload ? "" : "!");
					if (ff.sig_aload[0].wire == NULL)
						f << stringf("%s", sig_aload_name.c_str());
					else
						vhdl_dump_sigspec(f, ff.sig_aload);
					f << stringf(") %s <= ", reg_bit_name.c_str());
					vhdl_dump_sigspec(f, sig_ad);
					f << stringf(";\n");
					f << stringf("%s" "  else ", indent.c_str());
				}

				if (ff.has_srst && ff.has_ce && ff.ce_over_srst) {
					f << stringf("if (%s", ff.pol_ce ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_ce);
					f << stringf(")\n");
					f << stringf("%s" "    if (%s", indent.c_str(), ff.pol_srst ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_srst);
					f << stringf(") %s <= ", reg_bit_name.c_str());
					vhdl_dump_sigspec(f, val_srst);
					f << stringf(";\n");
					f << stringf("%s" "    else ", indent.c_str());
				} else {
					if (ff.has_srst) {
						f << stringf("if (%s", ff.pol_srst ? "" : "!");
						vhdl_dump_sigspec(f, ff.sig_srst);
						f << stringf(") %s <= ", reg_bit_name.c_str());
						vhdl_dump_sigspec(f, val_srst);
						f << stringf(";\n");
						f << stringf("%s" "  else ", indent.c_str());
					}
					if (ff.has_ce) {
						f << stringf("if (%s", ff.pol_ce ? "" : "!");
						vhdl_dump_sigspec(f, ff.sig_ce);
						f << stringf(") ");
					}
				}

				f << stringf("%s <= ", reg_bit_name.c_str());
				vhdl_dump_sigspec(f, sig_d);
				f << stringf(";\n");
			}
			else
			{
				// Latches.
				f << stringf("%s" "always%s\n", indent.c_str(), systemvhdl ? "_latch" : " @*");

				f << stringf("%s" "  ", indent.c_str());
				if (ff.has_sr) {
					f << stringf("if (%s", ff.pol_clr ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_clr[i]);
					f << stringf(") %s = 1'b0;\n", reg_bit_name.c_str());
					f << stringf("%s" "  else if (%s", indent.c_str(), ff.pol_set ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_set[i]);
					f << stringf(") %s = 1'b1;\n", reg_bit_name.c_str());
					if (ff.has_aload)
						f << stringf("%s" "  else ", indent.c_str());
				} else if (ff.has_arst) {
					f << stringf("if (%s", ff.pol_arst ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_arst);
					f << stringf(") %s = ", reg_bit_name.c_str());
					vhdl_dump_sigspec(f, val_arst);
					f << stringf(";\n");
					if (ff.has_aload)
						f << stringf("%s" "  else ", indent.c_str());
				}
				if (ff.has_aload) {
					f << stringf("if (%s", ff.pol_aload ? "" : "!");
					vhdl_dump_sigspec(f, ff.sig_aload);
					f << stringf(") %s = ", reg_bit_name.c_str());
					vhdl_dump_sigspec(f, sig_ad);
					f << stringf(";\n");
				}
			}
		}

		if (!out_is_reg_wire) {
			f << stringf("%s" "24assign ", indent.c_str());
			vhdl_dump_sigspec(f, ff.sig_q);
			f << stringf(" = %s;\n", reg_name.c_str());
		}

		return true;
	}

	if (cell->type.in(ID($assert), ID($assume), ID($cover)))
	{
		f << stringf("%s" "always%s if (", indent.c_str(), systemvhdl ? "_comb" : " @*");
		vhdl_dump_sigspec(f, cell->getPort(ID::EN));
		f << stringf(") %s(", cell->type.c_str()+1);
		vhdl_dump_sigspec(f, cell->getPort(ID::A));
		f << stringf(");\n");
		return true;
	}

	if (cell->type.in(ID($specify2), ID($specify3)))
	{
		f << stringf("%s" "specify\n%s  ", indent.c_str(), indent.c_str());

		SigSpec en = cell->getPort(ID::EN);
		if (en != State::S1) {
			f << stringf("if (");
			vhdl_dump_sigspec(f, cell->getPort(ID::EN));
			f << stringf(") ");
		}

		f << "(";
		if (cell->type == ID($specify3) && cell->getParam(ID::EDGE_EN).as_bool())
			f << (cell->getParam(ID::EDGE_POL).as_bool() ? "posedge ": "negedge ");

		vhdl_dump_sigspec(f, cell->getPort(ID::SRC));

		f << " ";
		if (cell->getParam(ID::SRC_DST_PEN).as_bool())
			f << (cell->getParam(ID::SRC_DST_POL).as_bool() ? "+": "-");
		f << (cell->getParam(ID::FULL).as_bool() ? "*> ": "=> ");

		if (cell->type == ID($specify3)) {
			f << "(";
			vhdl_dump_sigspec(f, cell->getPort(ID::DST));
			f << " ";
			if (cell->getParam(ID::DAT_DST_PEN).as_bool())
				f << (cell->getParam(ID::DAT_DST_POL).as_bool() ? "+": "-");
			f << ": ";
			vhdl_dump_sigspec(f, cell->getPort(ID::DAT));
			f << ")";
		} else {
			vhdl_dump_sigspec(f, cell->getPort(ID::DST));
		}

		bool bak_decimal = decimal;
		decimal = 1;

		f << ") = (";
		vhdl_dump_const(f, cell->getParam(ID::T_RISE_MIN));
		f << ":";
		vhdl_dump_const(f, cell->getParam(ID::T_RISE_TYP));
		f << ":";
		vhdl_dump_const(f, cell->getParam(ID::T_RISE_MAX));
		f << ", ";
		vhdl_dump_const(f, cell->getParam(ID::T_FALL_MIN));
		f << ":";
		vhdl_dump_const(f, cell->getParam(ID::T_FALL_TYP));
		f << ":";
		vhdl_dump_const(f, cell->getParam(ID::T_FALL_MAX));
		f << ");\n";

		decimal = bak_decimal;

		f << stringf("%s" "endspecify\n", indent.c_str());
		return true;
	}

	if (cell->type == ID($specrule))
	{
		f << stringf("%s" "specify\n%s  ", indent.c_str(), indent.c_str());

		IdString spec_type = cell->getParam(ID::TYPE).decode_string();
		f << stringf("%s(", spec_type.c_str());

		if (cell->getParam(ID::SRC_PEN).as_bool())
			f << (cell->getParam(ID::SRC_POL).as_bool() ? "posedge ": "negedge ");
		vhdl_dump_sigspec(f, cell->getPort(ID::SRC));

		if (cell->getPort(ID::SRC_EN) != State::S1) {
			f << " &&& ";
			vhdl_dump_sigspec(f, cell->getPort(ID::SRC_EN));
		}

		f << ", ";
		if (cell->getParam(ID::DST_PEN).as_bool())
			f << (cell->getParam(ID::DST_POL).as_bool() ? "posedge ": "negedge ");
		vhdl_dump_sigspec(f, cell->getPort(ID::DST));

		if (cell->getPort(ID::DST_EN) != State::S1) {
			f << " &&& ";
			vhdl_dump_sigspec(f, cell->getPort(ID::DST_EN));
		}

		bool bak_decimal = decimal;
		decimal = 1;

		f << ", ";
		vhdl_dump_const(f, cell->getParam(ID::T_LIMIT_MIN));
		f << ": ";
		vhdl_dump_const(f, cell->getParam(ID::T_LIMIT_TYP));
		f << ": ";
		vhdl_dump_const(f, cell->getParam(ID::T_LIMIT_MAX));

		if (spec_type.in(ID($setuphold), ID($recrem), ID($fullskew))) {
			f << ", ";
			vhdl_dump_const(f, cell->getParam(ID::T_LIMIT2_MIN));
			f << ": ";
			vhdl_dump_const(f, cell->getParam(ID::T_LIMIT2_TYP));
			f << ": ";
			vhdl_dump_const(f, cell->getParam(ID::T_LIMIT2_MAX));
		}

		f << ");\n";
		decimal = bak_decimal;

		f << stringf("%s" "endspecify\n", indent.c_str());
		return true;
	}

	// FIXME: $fsm

	return false;
}

void vhdl_dump_sigchunk_intermediate(std::ostream &f, const RTLIL::SigChunk &chunk, string portName,
                                     int& left, int& right, bool no_decimal = false)
{
    int width = 0;

    if (chunk.wire == NULL) {

         // Impossible
         //
         vhdl_dump_const(f, chunk.data, chunk.width, chunk.offset, no_decimal);

    } else {

         if (chunk.width == chunk.wire->width && chunk.offset == 0) {

             width = chunk.width - 1;

             f << stringf("%s", id(chunk.wire->name).c_str());

         } else if (chunk.width == 1) {

             width = 1;

             if (chunk.wire->upto) {

                 f << stringf("%s(%d)", id(chunk.wire->name).c_str(), 
                                             (chunk.wire->width - chunk.offset - 1) + 
                                             chunk.wire->start_offset);
             } else {
                 f << stringf("%s(%d)", id(chunk.wire->name).c_str(), 
                                             chunk.offset + chunk.wire->start_offset);
             }
         } else {
               if (chunk.wire->upto) {
                  f << stringf("%s(%d upto %d)", id(chunk.wire->name).c_str(),
                                             (chunk.wire->width - 
                                             (chunk.offset + chunk.width - 1) - 1) + 
                                             chunk.wire->start_offset, (chunk.wire->width - 
                                             chunk.offset - 1) + chunk.wire->start_offset);

                  width = ((chunk.wire->width -chunk.offset - 1) + chunk.wire->start_offset) -
                          ((chunk.wire->width - (chunk.offset + chunk.width - 1) - 1) + 
                           chunk.wire->start_offset); 
               } else {
                      f << stringf("%s(%d downto %d)", id(chunk.wire->name).c_str(),
                                            (chunk.offset + chunk.width - 1) + chunk.wire->start_offset,                                            chunk.offset + chunk.wire->start_offset);
                     width = (chunk.offset + chunk.width - 1) + chunk.wire->start_offset - 
                             (chunk.offset + chunk.wire->start_offset);
              }
         }
     }

     f << " <= ";

     if (left >= right) {
        int newLeft = left - width ;
        f << stringf("%s (%d downto %d) ", portName.c_str(), left, newLeft);
        left = newLeft-1;
     } else {
        int newLeft = left + width ;
        f << stringf("%s (%d upto %d) ", portName.c_str(), left, newLeft);
        left = newLeft+1;
     }
}

void processIntermediateAssignment(std::ostream &f, const string &portName,
                                   int left, int right, const RTLIL::SigSpec &actual)
{
// titi
     if (actual.is_chunk()) {

        f << "  ";
        vhdl_dump_sigchunk(f, actual.as_chunk(), false /* no_decimal*/ );
        f << stringf(" <= %s ;\n", portName.c_str());

     } else {
        f << " \n";
        f << "  -- From output concat expression\n";
        for (auto it = actual.chunks().rbegin(); it != actual.chunks().rend(); ++it) {
           f << "  ";
           vhdl_dump_sigchunk_intermediate(f, *it, portName, left, right, true);
           f << ";\n";
        }
        f << " \n";
     }
}

void vhdl_dump_cell_intermediate_assignments(std::ostream &f, RTLIL::Cell *cell)
{
     std::string instanceName = cellname(cell);

     string cellName = id(cell->type, false);

     if (cellName == "TDP36K") {
	for (auto it = cell->connections().begin(); it != cell->connections().end(); ++it) {
           const RTLIL::SigSpec actual = it->second;

           if (id(it->first) == "RDATA_A1_o") {
             string portName = instanceName + "_" + id(it->first);
             processIntermediateAssignment (f, portName, 17, 0, actual);
             continue;
           }
           if (id(it->first) == "RDATA_B1_o") {
             string portName = instanceName + "_" + id(it->first);
             processIntermediateAssignment (f, portName, 17, 0, actual);
             continue;
           }
           if (id(it->first) == "RDATA_A2_o") {
             string portName = instanceName + "_" + id(it->first);
             processIntermediateAssignment (f, portName, 17, 0, actual);
             continue;
           }
           if (id(it->first) == "RDATA_B2_o") {
             string portName = instanceName + "_" + id(it->first);
             processIntermediateAssignment (f, portName, 17, 0, actual);
             continue;
           }
        }
        return;
     }

     if (cellName == "RS_DSP2_MULT") {
	for (auto it = cell->connections().begin(); it != cell->connections().end(); ++it) {

           const RTLIL::SigSpec actual = it->second;

           if (id(it->first) == "z") {
             string portName = instanceName + "_" + id(it->first);
             processIntermediateAssignment (f, portName, 37, 0, actual);
             continue;
           }
        }
        return;
     }

}

void vhdl_dump_cell_intermediate_outputs(std::ostream &f, std::string indent, RTLIL::Cell *cell)
{
     std::string instanceName = cellname(cell);

     string cellName = id(cell->type, false);

     if (cellName == "TDP36K") {
       f << stringf("%s" "signal %s_RDATA_A1_o : std_logic_vector (17 downto 0);\n", indent.c_str(),
                    instanceName.c_str());
       f << stringf("%s" "signal %s_RDATA_B1_o : std_logic_vector (17 downto 0);\n", indent.c_str(),
                    instanceName.c_str());
       f << stringf("%s" "signal %s_RDATA_A2_o : std_logic_vector (17 downto 0);\n", indent.c_str(),
                    instanceName.c_str());
       f << stringf("%s" "signal %s_RDATA_B2_o : std_logic_vector (17 downto 0);\n", indent.c_str(),
                    instanceName.c_str());
       return;
     }

     if (cellName == "RS_DSP2_MULT") {
       f << stringf("%s" "signal %s_z : std_logic_vector (37 downto 0);\n", indent.c_str(),
                    instanceName.c_str());
       return;
     }
}

bool isGenesis2(string cellName) 
{
if (cellName == "dff")
       return true;
   if (cellName == "dffn")
       return true;
   if (cellName == "sdff")
       return true;
   if (cellName == "sdffn")
       return true;
   if (cellName == "dffr")
       return true;
   if (cellName == "dffnr")
       return true;
   if (cellName == "dffe")
       return true;
   if (cellName == "dffne")
       return true;
   if (cellName == "sdffre")
       return true;
   if (cellName == "sdffnre")
       return true;
   if (cellName == "dffre")
       return true;
   if (cellName == "dffnre")
       return true;
   if (cellName == "latch")
       return true;
   if (cellName == "latchn")
       return true;
   if (cellName == "latchr")
       return true;
   if (cellName == "latchnr")
       return true;
   if (cellName == "fa_1bit")
       return true;

   return false;
}

bool unsupportedCell(string cellName)
{
   if (cellName == "shr") 
       return false;

   if (cellName == "adder_carry") 
       return false;

   if (cellName == "TDP36K") 
       return false;

   if (cellName == "RS_DSP2_MULT") 
       return false;

   if (cellName == "sh_dff") 
       return false;

   if (cellName == "latchsre") 
       return false;

   if (cellName == "dffsre") 
       return false;

   if (cellName == "dffnsre") 
       return false;

   /* Genesis 2 */
   if (isGenesis2(cellName))
     return false;

   return true;
}

void vhdl_dump_cell(std::ostream &f, std::string indent, RTLIL::Cell *cell)
{
	// Handled by vhdl_dump_memory
	if (cell->is_mem_cell()) {
           return;
	}

	if (cell->type[0] == '$' && !noexpr) {
           if (vhdl_dump_cell_expr(f, indent, cell)) {
              return;
	   }
	}

	string cellName = id(cell->type, false);

	// Thierry : customized only for structural VHDL with specific cells
	//
        if (unsupportedCell(cellName)) {

           log_cmd_error("Error: write_vhdl cannot handle cell '%s'\n", cellName.c_str());
	}

	// Thierry : perform regular clean up of new created legalized strings
	//
        cleanUpNewStrings();

	vhdl_dump_attributes(f, indent, cell->attributes);

        // Print instance name and cell name to be instantiated
	//
	f << stringf("%s" "%s : %s \n", indent.c_str(), 
                     cellname(cell).c_str(), cellName.c_str());

	// ================================================
	// Process "generic map" part of cell instantiation
	//
        if (0 || isGenesis2(cellName)) {

	  if (!defparam && cell->parameters.size() > 0) {
		f << stringf("     generic map (");
		for (auto it = cell->parameters.begin(); it != cell->parameters.end(); ++it) {
			if (it != cell->parameters.begin())
				f << stringf(" ,");
			f << stringf("\n    %s  %s => ", indent.c_str(), id(it->first).c_str());
			vhdl_dump_const(f, it->second);
		}
		f << stringf("\n%s" "   )\n", indent.c_str());
          }
        }

	// ================================================
	// Processing "port map" association in instance
	//
	std::string cell_name = cellname(cell);
	if (cell_name != id(cell->name))
		f << stringf(" %s /* %s */ (", cell_name.c_str(), id(cell->name).c_str());
	else
		f << stringf("     port map (");

	bool first_arg = true;
	std::set<RTLIL::IdString> numbered_ports;

	for (int i = 1; true; i++) {
		char str[16];
		snprintf(str, 16, "$%d", i);
		for (auto it = cell->connections().begin(); it != cell->connections().end(); ++it) {
			if (it->first != str)
				continue;
			if (!first_arg)
				f << stringf(",");
			first_arg = false;
			f << stringf("\n%s  ", indent.c_str());
			vhdl_dump_sigspec(f, it->second);
			numbered_ports.insert(it->first);
			goto found_numbered_port;
		}
		break;
	found_numbered_port:;
	}

	// Processing the "=>" associations in instance
	//
	for (auto it = cell->connections().begin(); it != cell->connections().end(); ++it) {

		if (numbered_ports.count(it->first))
			continue;

		if (!first_arg)
			f << stringf(" ,");

		first_arg = false;

		// Print the port name
		//
		string portName = id(it->first);
		f << stringf("\n%s     %s => ", indent.c_str(), portName.c_str());

		if (it->second.size() > 0) {
                   // Thierry :
                   // Do not use decimals in case of constants, use std logic vectors
		   //
                   bool vectorize = false;
		   if (cellName == "shr") {

		     if (portName == "A") {
                        vectorize = true;
		     }
		     if (portName == "B") {
                        vectorize = true;
		     }
		   }

		   if (vectorize) {
                     f << stringf("\"\" & ");
		   }

		   if ((cellName == "TDP36K") &&
                       (portName == "RDATA_A1_o")) {
                     f << stringf("%s_%s", cell_name.c_str(), portName.c_str());
                     continue;
                   }
		   if ((cellName == "TDP36K") &&
                       (portName == "RDATA_B1_o")) {
                     f << stringf("%s_%s", cell_name.c_str(), portName.c_str());
                     continue;
                   }
		   if ((cellName == "TDP36K") &&
                       (portName == "RDATA_A2_o")) {
                     f << stringf("%s_%s", cell_name.c_str(), portName.c_str());
                     continue;
                   }
		   if ((cellName == "TDP36K") &&
                       (portName == "RDATA_B2_o")) {
                     f << stringf("%s_%s", cell_name.c_str(), portName.c_str());
                     continue;
                   }

		   if ((cellName == "RS_DSP2_MULT") &&
                       (portName == "z")) {
                     f << stringf("%s_%s", cell_name.c_str(), portName.c_str());
                     continue;
                   }

                   vhdl_dump_sigspec(f, it->second, true /* no_decimal !!! */);
		}
	}
	f << stringf("\n%s" "   );\n", indent.c_str());

	if (defparam && cell->parameters.size() > 0) {
		for (auto it = cell->parameters.begin(); it != cell->parameters.end(); ++it) {
			f << stringf("%sdefparam %s.%s = ", indent.c_str(), cell_name.c_str(), 
					id(it->first).c_str());
			vhdl_dump_const(f, it->second);
			f << stringf(";\n");
		}
	}

	if (siminit && RTLIL::builtin_ff_cell_types().count(cell->type) && 
			cell->hasPort(ID::Q) && !cell->type.in(ID($ff), ID($_FF_))) {
		std::stringstream ss;
		vhdl_dump_reg_init(ss, cell->getPort(ID::Q));
		if (!ss.str().empty()) {
			f << stringf("%sinitial %s.Q", indent.c_str(), cell_name.c_str());
			f << ss.str();
			f << ";\n";
		}
	}
}

void vhdl_dump_conn(std::ostream &f, std::string indent, const RTLIL::SigSpec &left, const RTLIL::SigSpec &right)
{
	// [Thierry] : force simple LHS assignments in VHDL
	//
	if (1 || simple_lhs) {
		int offset = 0;
		for (auto &chunk : left.chunks()) {
			f << stringf("%s" "", indent.c_str());
			vhdl_dump_sigspec(f, chunk);
			f << stringf(" <= ");
			vhdl_dump_sigspec(f, right.extract(offset, GetSize(chunk)));
			f << stringf(";\n");
			offset += GetSize(chunk);
		}
	} else {
		f << stringf("%s" "", indent.c_str());
		vhdl_dump_sigspec(f, left);
		f << stringf(" <= ");
		vhdl_dump_sigspec(f, right);
		f << stringf(";\n");
	}
}

void vhdl_dump_proc_switch(std::ostream &f, std::string indent, RTLIL::SwitchRule *sw);

void vhdl_dump_case_body(std::ostream &f, std::string indent, RTLIL::CaseRule *cs, bool omit_trailing_begin = false)
{
	int number_of_stmts = cs->switches.size() + cs->actions.size();

	if (!omit_trailing_begin && number_of_stmts >= 2)
		f << stringf("%s" "begin\n", indent.c_str());

	for (auto it = cs->actions.begin(); it != cs->actions.end(); ++it) {
		if (it->first.size() == 0)
			continue;
		f << stringf("%s  ", indent.c_str());
		vhdl_dump_sigspec(f, it->first);
		f << stringf(" = ");
		vhdl_dump_sigspec(f, it->second);
		f << stringf(";\n");
	}

	for (auto it = cs->switches.begin(); it != cs->switches.end(); ++it)
		vhdl_dump_proc_switch(f, indent + "  ", *it);

	if (!omit_trailing_begin && number_of_stmts == 0)
		f << stringf("%s  /* empty */;\n", indent.c_str());

	if (omit_trailing_begin || number_of_stmts >= 2)
		f << stringf("%s" "end\n", indent.c_str());
}

void vhdl_dump_proc_switch(std::ostream &f, std::string indent, RTLIL::SwitchRule *sw)
{
	if (sw->signal.size() == 0) {
		f << stringf("%s" "begin\n", indent.c_str());
		for (auto it = sw->cases.begin(); it != sw->cases.end(); ++it) {
			if ((*it)->compare.size() == 0)
				vhdl_dump_case_body(f, indent + "  ", *it);
		}
		f << stringf("%s" "end\n", indent.c_str());
		return;
	}

	vhdl_dump_attributes(f, indent, sw->attributes);
	f << stringf("%s" "casez (", indent.c_str());
	vhdl_dump_sigspec(f, sw->signal);
	f << stringf(")\n");

	bool got_default = false;
	for (auto it = sw->cases.begin(); it != sw->cases.end(); ++it) {
		vhdl_dump_attributes(f, indent + "  ", (*it)->attributes, '\n', /*modattr=*/false, /*regattr=*/false, /*as_comment=*/true);
		if ((*it)->compare.size() == 0) {
			if (got_default)
				continue;
			f << stringf("%s  default", indent.c_str());
			got_default = true;
		} else {
			f << stringf("%s  ", indent.c_str());
			for (size_t i = 0; i < (*it)->compare.size(); i++) {
				if (i > 0)
					f << stringf(", ");
				vhdl_dump_sigspec(f, (*it)->compare[i]);
			}
		}
		f << stringf(":\n");
		vhdl_dump_case_body(f, indent + "    ", *it);
	}

	f << stringf("%s" "endcase\n", indent.c_str());
}

void case_body_find_regs(RTLIL::CaseRule *cs)
{
	for (auto it = cs->switches.begin(); it != cs->switches.end(); ++it)
	for (auto it2 = (*it)->cases.begin(); it2 != (*it)->cases.end(); it2++)
		case_body_find_regs(*it2);

	for (auto it = cs->actions.begin(); it != cs->actions.end(); ++it) {
		for (auto &c : it->first.chunks())
			if (c.wire != NULL)
				reg_wires.insert(c.wire->name);
	}
}

void vhdl_dump_process(std::ostream &f, std::string indent, RTLIL::Process *proc, bool find_regs = false)
{
	if (find_regs) {
		case_body_find_regs(&proc->root_case);
		for (auto it = proc->syncs.begin(); it != proc->syncs.end(); ++it)
		for (auto it2 = (*it)->actions.begin(); it2 != (*it)->actions.end(); it2++) {
			for (auto &c : it2->first.chunks())
				if (c.wire != NULL)
					reg_wires.insert(c.wire->name);
		}
		return;
	}

	f << stringf("%s" "always%s begin\n", indent.c_str(), systemvhdl ? "_comb" : " @*");
	if (!systemvhdl)
		f << indent + "  " << "if (" << id(initial_id) << ") begin end\n";
	vhdl_dump_case_body(f, indent, &proc->root_case, true);

	std::string backup_indent = indent;

	for (size_t i = 0; i < proc->syncs.size(); i++)
	{
		RTLIL::SyncRule *sync = proc->syncs[i];
		indent = backup_indent;

		if (sync->type == RTLIL::STa) {
			f << stringf("%s" "always%s begin\n", indent.c_str(), systemvhdl ? "_comb" : " @*");
		} else if (sync->type == RTLIL::STi) {
			f << stringf("%s" "initial begin\n", indent.c_str());
		} else {
			f << stringf("%s" "always%s @(", indent.c_str(), systemvhdl ? "_ff" : "");
			if (sync->type == RTLIL::STp || sync->type == RTLIL::ST1)
				f << stringf("posedge ");
			if (sync->type == RTLIL::STn || sync->type == RTLIL::ST0)
				f << stringf("negedge ");
			vhdl_dump_sigspec(f, sync->signal);
			f << stringf(") begin\n");
		}
		std::string ends = indent + "end\n";
		indent += "  ";

		if (sync->type == RTLIL::ST0 || sync->type == RTLIL::ST1) {
			f << stringf("%s" "if (%s", indent.c_str(), sync->type == RTLIL::ST0 ? "!" : "");
			vhdl_dump_sigspec(f, sync->signal);
			f << stringf(") begin\n");
			ends = indent + "end\n" + ends;
			indent += "  ";
		}

		if (sync->type == RTLIL::STp || sync->type == RTLIL::STn) {
			for (size_t j = 0; j < proc->syncs.size(); j++) {
				RTLIL::SyncRule *sync2 = proc->syncs[j];
				if (sync2->type == RTLIL::ST0 || sync2->type == RTLIL::ST1) {
					f << stringf("%s" "if (%s", indent.c_str(), sync2->type == RTLIL::ST1 ? "!" : "");
					vhdl_dump_sigspec(f, sync2->signal);
					f << stringf(") begin\n");
					ends = indent + "end\n" + ends;
					indent += "  ";
				}
			}
		}

		for (auto it = sync->actions.begin(); it != sync->actions.end(); ++it) {
			if (it->first.size() == 0)
				continue;
			f << stringf("%s  ", indent.c_str());
			vhdl_dump_sigspec(f, it->first);
			f << stringf(" <= ");
			vhdl_dump_sigspec(f, it->second);
			f << stringf(";\n");
		}

		f << stringf("%s", ends.c_str());
	}
}

void printComponent_TDP36K(std::ostream &f, std::string indent)
{
	f << stringf("%s" "component TDP36K\n", indent.c_str());
#if 0
        f << stringf("%s" " generic (\n", indent.c_str());
        f << stringf("%s" "    MODE_BITS : std_logic_vector (80 downto 0);\n", indent.c_str());
        f << stringf("%s" "    INIT_i : std_ulogic_vector (36863 downto 0)\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
#endif
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    RESET_ni : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    WEN_A1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    WEN_B1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    REN_A1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    REN_B1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    CLK_A1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    CLK_B1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    BE_A1_i : in std_logic_vector (1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    BE_B1_i : in std_logic_vector (1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    ADDR_A1_i : in std_logic_vector (14 downto 0);\n", indent.c_str());
        f << stringf("%s" "    ADDR_B1_i : in std_logic_vector (14 downto 0);\n", indent.c_str());
        f << stringf("%s" "    WDATA_A1_i : in std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    WDATA_B1_i : in std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    RDATA_A1_o : out std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    RDATA_B1_o : out std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    FLUSH1_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    WEN_A2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    WEN_B2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    REN_A2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    REN_B2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    CLK_A2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    CLK_B2_i : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    BE_A2_i : in std_logic_vector (1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    BE_B2_i : in std_logic_vector (1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    ADDR_A2_i : in std_logic_vector (13 downto 0);\n", indent.c_str());
        f << stringf("%s" "    ADDR_B2_i : in std_logic_vector (13 downto 0);\n", indent.c_str());
        f << stringf("%s" "    WDATA_A2_i : in std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    WDATA_B2_i : in std_ulogic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    RDATA_A2_o : out std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    RDATA_B2_o : out std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    FLUSH2_i : in std_logic\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_RS_DSP2_MULT(std::ostream &f, std::string indent)
{
        f << stringf("%s" "component RS_DSP2_MULT\n", indent.c_str());
#if 0
        f << stringf("%s" " generic (\n", indent.c_str());
        f << stringf("%s" "    MODE_BITS : std_logic_vector (79 downto 0)\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
#endif
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    a : in std_logic_vector (19 downto 0);\n", indent.c_str());
        f << stringf("%s" "    b : in std_logic_vector (17 downto 0);\n", indent.c_str());
        f << stringf("%s" "    z : out std_logic_vector (37 downto 0);\n", indent.c_str());
        f << stringf("%s" "    reset : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    feedback : in std_logic_vector(2 downto 0);\n", indent.c_str());
        f << stringf("%s" "    unsigned_a : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    unsigned_b : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    f_mode : in std_logic;\n", indent.c_str());
        f << stringf("%s" "    output_select : in std_logic_vector(2 downto 0);\n", indent.c_str());
        f << stringf("%s" "    register_inputs : in std_logic\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_lut(std::ostream &f, std::string indent)
{
        f << stringf("%s" "component lut\n", indent.c_str());
        f << stringf("%s" " generic (\n", indent.c_str());
        f << stringf("%s" "    A_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    B_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    Y_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    A_SIGNED : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    B_SIGNED : INTEGER \n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Y : out std_logic;\n", indent.c_str());
        f << stringf("%s" "    B : in std_logic_vector(B_WIDTH-1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    A : in std_logic_vector(A_WIDTH-1 downto 0)\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_shr(std::ostream &f, std::string indent)
{
        f << stringf("%s" "component shr\n", indent.c_str());
        f << stringf("%s" " generic (\n", indent.c_str());
        f << stringf("%s" "    A_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    B_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    Y_WIDTH : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    A_SIGNED : INTEGER ;\n", indent.c_str());
        f << stringf("%s" "    B_SIGNED : INTEGER \n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Y : out std_logic_vector(Y_WIDTH-1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    B : in std_logic_vector(B_WIDTH-1 downto 0);\n", indent.c_str());
        f << stringf("%s" "    A : in std_logic_vector(A_WIDTH-1 downto 0)\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_dffsre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffsre\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    S : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_dffnsre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffnsre\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    S : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

void printComponent_latchsre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component latchsre\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    S : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    G : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}


void printComponent_adder_carry(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component adder_carry\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    sumout : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    cout : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    p : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    cin : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}

// Genesis 2
//
//
void printComponent_dff(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dff\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffn(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffn\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_sdff(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component sdff\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_sdffn(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component sdffn\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffr(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffr\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffnr(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffnr\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffe(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffe\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffne(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffne\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_sdffre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component sdffre\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_sdffnre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component sdffnre\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffre\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_dffnre(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component dffnre\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    R : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    E : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    D : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    C : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_latch(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component latch\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    d : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_latchn(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component latchn\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    d : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_latchr(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component latchr\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    r : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    d : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_latchnr(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component latchnr\n", indent.c_str());
        f << stringf("%s" "  generic (INIT_VALUE : std_logic := '0');\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    Q : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    r : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    d : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}
void printComponent_fa_1bit(std::ostream &f, std::string indent)
{
        f << stringf("%s" " component fa_1bit\n", indent.c_str());
        f << stringf("%s" "  port (\n", indent.c_str());
        f << stringf("%s" "    sum : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    cout : out std_logic ;\n", indent.c_str());
        f << stringf("%s" "    p : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    g : in std_logic := '0';\n", indent.c_str());
        f << stringf("%s" "    cin : in std_logic := '0'\n", indent.c_str());
        f << stringf("%s" "  );\n", indent.c_str());
        f << stringf("%s" " end component;\n", indent.c_str());
}


void vhdl_dump_module(std::ostream &f, std::string indent, RTLIL::Module *module)
{
	reg_wires.clear();
	reset_auto_counter(module);
	active_module = module;
	active_sigmap.set(module);
	active_initdata.clear();

	for (auto wire : module->wires())
		if (wire->attributes.count(ID::init)) {
			SigSpec sig = active_sigmap(wire);
			Const val = wire->attributes.at(ID::init);
			for (int i = 0; i < GetSize(sig) && i < GetSize(val); i++)
				if (val[i] == State::S0 || val[i] == State::S1)
					active_initdata[sig[i]] = val[i];
		}

	if (!module->processes.empty())
		log_warning("Module %s contains unmapped RTLIL processes. RTLIL processes\n"
				"can't always be mapped directly to Vhdl always blocks. Unintended\n"
				"changes in simulation behavior are possible! Use \"proc\" to convert\n"
				"processes to logic networks and registers.\n", log_id(module));

	f << stringf("\n");
	for (auto it = module->processes.begin(); it != module->processes.end(); ++it)
		vhdl_dump_process(f, indent + "  ", it->second, true);

	if (!noexpr)
	{
		std::set<std::pair<RTLIL::Wire*,int>> reg_bits;
		for (auto cell : module->cells())
		{
			if (!RTLIL::builtin_ff_cell_types().count(cell->type) || !cell->hasPort(ID::Q) || cell->type.in(ID($ff), ID($_FF_)))
				continue;

			RTLIL::SigSpec sig = cell->getPort(ID::Q);

			if (sig.is_chunk()) {
				RTLIL::SigChunk chunk = sig.as_chunk();
				if (chunk.wire != NULL)
					for (int i = 0; i < chunk.width; i++)
						reg_bits.insert(std::pair<RTLIL::Wire*,int>(chunk.wire, chunk.offset+i));
			}
		}
		for (auto wire : module->wires())
		{
			for (int i = 0; i < wire->width; i++)
				if (reg_bits.count(std::pair<RTLIL::Wire*,int>(wire, i)) == 0)
					goto this_wire_aint_reg;
			if (wire->width)
				reg_wires.insert(wire->name);
		this_wire_aint_reg:;
		}
	}

	vhdl_dump_attributes(f, indent, module->attributes, '\n', /*modattr=*/true);

	bool keep_running = true;
	std::string range = "";
	int cnt = 0;

	f << stringf("%s" "entity %s is \n", indent.c_str(), id(module->name, false).c_str());
	f << stringf("%s" "  port (\n", indent.c_str());
	for (int port_id = 1; keep_running; port_id++) {
		keep_running = false;
		for (auto wire : module->wires()) {
			if (wire->port_id == port_id) {
				if (port_id != 1) {
					f << stringf(" ; \n");
				}

				f << stringf("     %s :", id(wire->name).c_str());

				if (wire->start_offset == wire->width - 1 + wire->start_offset) {
			           range = stringf("std_logic"); 
				} else if (wire->upto)
			           range = stringf("std_logic_vector(%d upto %d)", 
						   wire->start_offset, wire->width - 1 + wire->start_offset);
		                else
			           range = stringf("std_logic_vector(%d downto %d)", 
						   wire->width - 1 + wire->start_offset, wire->start_offset);

				if (wire->port_input && !wire->port_output)
					f << stringf(" in %s", range.c_str());

				if (!wire->port_input && wire->port_output)
					f << stringf(" out %s", range.c_str());

				if (wire->port_input && wire->port_output)
					f << stringf(" inout %s", range.c_str());

				keep_running = true;
				if (cnt==20) { f << stringf("\n"); cnt = 0; } else cnt++;
				continue;
			}
		}
	}
	f << stringf("\n  );\n");
	f << stringf("%s" "end %s;\n\n", indent.c_str(), id(module->name, false).c_str());

	if (!systemvhdl && !module->processes.empty()) {
		initial_id = NEW_ID;
		f << indent + "  " << "reg " << id(initial_id) << " = 0;\n";
	}

	f << stringf("%s" "architecture arch of %s is\n", indent.c_str(), id(module->name, false).c_str());

        // Genesis 
        //
        printComponent_TDP36K(f, indent);

        printComponent_RS_DSP2_MULT(f, indent);

        printComponent_lut(f, indent);

        printComponent_shr(f, indent);

        printComponent_dffsre(f, indent);

        printComponent_dffnsre(f, indent);

        printComponent_latchsre(f, indent);

        printComponent_adder_carry(f, indent);

        // Genesis 2
        //
        printComponent_dff(f, indent);
        printComponent_dffn(f, indent);
        printComponent_sdff(f, indent);
        printComponent_sdffn(f, indent);
        printComponent_dffr(f, indent);
        printComponent_dffnr(f, indent);
        printComponent_dffe(f, indent);
        printComponent_dffne(f, indent);
        printComponent_sdffre(f, indent);
        printComponent_sdffnre(f, indent);
        printComponent_dffre(f, indent);
        printComponent_dffnre(f, indent);
        printComponent_latch(f, indent);
        printComponent_latchn(f, indent);
        printComponent_latchr(f, indent);
        printComponent_latchnr(f, indent);
        printComponent_fa_1bit(f, indent);

	for (auto w : module->wires())
		vhdl_dump_signal(f, indent + "  ", w);

        // Dump intermediate signals for specific cell outputs
	for (auto cell : module->cells()) {
		vhdl_dump_cell_intermediate_outputs(f, indent + "  ", cell);
        }

	f << stringf("\n");

	for (auto &mem : Mem::get_all_memories(module)) {
	    log_cmd_error("do not support memories for now\n");
	    vhdl_dump_memory(f, indent + "  ", mem);
	}

        f << stringf("%s" "begin\n", indent.c_str());

	for (auto cell : module->cells()) {
		vhdl_dump_cell(f, indent + "  ", cell);
	}

	for (auto it = module->processes.begin(); it != module->processes.end(); ++it)
		vhdl_dump_process(f, indent + "  ", it->second);

	for (auto it = module->connections().begin(); it != module->connections().end(); ++it)
		vhdl_dump_conn(f, indent + "  ", it->first, it->second);

	for (auto cell : module->cells()) {
		vhdl_dump_cell_intermediate_assignments(f, cell);
	}

	f << stringf("%s" "end arch;\n", indent.c_str());

	active_module = NULL;
	active_sigmap.clear();
	active_initdata.clear();
}

struct VhdlBackend : public Backend {
	VhdlBackend() : Backend("vhdl", "write design to Vhdl file") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_vhdl [options] [filename]\n");
		log("\n");
		log("Write the current design to a Vhdl file.\n");
		log("\n");
		log("    -sv\n");
		log("        with this option, SystemVhdl constructs like always_comb are used\n");
		log("\n");
		log("    -norename\n");
		log("        without this option all internal object names (the ones with a dollar\n");
		log("        instead of a backslash prefix) are changed to short names in the\n");
		log("        format '_<number>_'.\n");
		log("\n");
		log("    -renameprefix <prefix>\n");
		log("        insert this prefix in front of auto-generated instance names\n");
		log("\n");
		log("    -noattr\n");
		log("        with this option no attributes are included in the output\n");
		log("\n");
		log("    -attr2comment\n");
		log("        with this option attributes are included as comments in the output\n");
		log("\n");
		log("    -noexpr\n");
		log("        without this option all internal cells are converted to Vhdl\n");
		log("        expressions.\n");
		log("\n");
		log("    -siminit\n");
		log("        add initial statements with hierarchical refs to initialize FFs when\n");
		log("        in -noexpr mode.\n");
		log("\n");
		log("    -nodec\n");
		log("        32-bit constant values are by default vhdl_dumped as decimal numbers,\n");
		log("        not bit pattern. This option deactivates this feature and instead\n");
		log("        will write out all constants in binary.\n");
		log("\n");
		log("    -decimal\n");
		log("        vhdl_dump 32-bit constants in decimal and without size and radix\n");
		log("\n");
		log("    -nohex\n");
		log("        constant values that are compatible with hex output are usually\n");
		log("        vhdl_dumped as hex values. This option deactivates this feature and\n");
		log("        instead will write out all constants in binary.\n");
		log("\n");
		log("    -nostr\n");
		log("        Parameters and attributes that are specified as strings in the\n");
		log("        original input will be output as strings by this back-end. This\n");
		log("        deactivates this feature and instead will write string constants\n");
		log("        as binary numbers.\n");
		log("\n");
		log("    -simple-lhs\n");
		log("        Connection assignments with simple left hand side without concatenations.\n");
		log("\n");
		log("    -extmem\n");
		log("        instead of initializing memories using assignments to individual\n");
		log("        elements, use the '$readmemh' function to read initialization data\n");
		log("        from a file. This data is written to a file named by appending\n");
		log("        a sequential index to the Vhdl filename and replacing the extension\n");
		log("        with '.mem', e.g. 'write_vhdl -extmem foo.v' writes 'foo-1.mem',\n");
		log("        'foo-2.mem' and so on.\n");
		log("\n");
		log("    -defparam\n");
		log("        use 'defparam' statements instead of the Vhdl-2001 syntax for\n");
		log("        cell parameters.\n");
		log("\n");
		log("    -blackboxes\n");
		log("        usually modules with the 'blackbox' attribute are ignored. with\n");
		log("        this option set only the modules with the 'blackbox' attribute\n");
		log("        are written to the output file.\n");
		log("\n");
		log("    -selected\n");
		log("        only write selected modules. modules must be selected entirely or\n");
		log("        not at all.\n");
		log("\n");
		log("    -enableopt\n");
		log("        perform additional optimizations before writing Vhdl\n");
		log("\n");
		log("    -v\n");
		log("        verbose output (print new names of all renamed wires and cells)\n");
		log("\n");
		log("Note that RTLIL processes can't always be mapped directly to Vhdl\n");
		log("always blocks. This frontend should only be used to export an RTLIL\n");
		log("netlist, i.e. after the \"proc\" pass has been used to convert all\n");
		log("processes to logic networks and registers. A warning is generated when\n");
		log("this command is called on a design with RTLIL processes.\n");
		log("\n");
	}
	void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing Vhdl backend.\n");

		verbose = false;
		enableopt = false;
		norename = false;
		noattr = true;
		attr2comment = false;
		noexpr = false;
		nodec = false;
		nohex = true;
		nostr = false;
		extmem = false;
		defparam = false;
		decimal = true;
		siminit = false;
		simple_lhs = false;
		auto_prefix = "";

		bool blackboxes = false;
		bool selected = false;

		auto_name_map.clear();
		reg_wires.clear();

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-sv") {
				systemvhdl = true;
				continue;
			}
			if (arg == "-norename") {
				norename = true;
				continue;
			}
			if (arg == "-renameprefix" && argidx+1 < args.size()) {
				auto_prefix = args[++argidx];
				continue;
			}
			if (arg == "-noattr") {
				noattr = true;
				continue;
			}
			if (arg == "-attr2comment") {
				attr2comment = true;
				continue;
			}
			if (arg == "-noexpr") {
				noexpr = true;
				continue;
			}
			if (arg == "-nodec") {
				nodec = true;
				continue;
			}
			if (arg == "-nohex") {
				nohex = true;
				continue;
			}
			if (arg == "-nostr") {
				nostr = true;
				continue;
			}
			if (arg == "-extmem") {
				extmem = true;
				extmem_counter = 1;
				continue;
			}
			if (arg == "-defparam") {
				defparam = true;
				continue;
			}
			if (arg == "-decimal") {
				decimal = true;
				continue;
			}
			if (arg == "-siminit") {
				siminit = true;
				continue;
			}
			if (arg == "-blackboxes") {
				blackboxes = true;
				continue;
			}
			if (arg == "-selected") {
				selected = true;
				continue;
			}
			if (arg == "-simple-lhs") {
				simple_lhs = true;
				continue;
			}
			if (arg == "-v") {
				verbose = true;
				continue;
			}
			if (arg == "-enableopt") {
				enableopt = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);
		if (extmem)
		{
			if (filename == "<stdout>")
				log_cmd_error("Option -extmem must be used with a filename.\n");
			extmem_prefix = filename.substr(0, filename.rfind('.'));
		}

		if (enableopt)
		{
			log_push();
			Pass::call(design, "bmuxmap");
			Pass::call(design, "demuxmap");
			Pass::call(design, "clean_zerowidth");
			log_pop();

			design->sort();
		}

		*f << stringf("/* Generated by %s/Rapid Silicon */\n", yosys_version_str);
		*f << stringf("library ieee;\n");
                *f << stringf("use ieee.std_logic_1164.all;\n");
                *f << stringf("use ieee.numeric_std.all;\n");

		for (auto module : design->modules()) {
			if (module->get_blackbox_attribute() != blackboxes)
				continue;
			if (selected && !design->selected_whole_module(module->name)) {
				if (design->selected_module(module->name))
					log_cmd_error("Can't handle partially selected module %s!\n", log_id(module->name));
				continue;
			}
			log("Dumping module `%s'.\n", module->name.c_str());
			vhdl_dump_module(*f, "", module);
		}

		auto_name_map.clear();
		reg_wires.clear();
	}
} VhdlBackend;

PRIVATE_NAMESPACE_END
