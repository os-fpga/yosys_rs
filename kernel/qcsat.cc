/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2021  Marcelina Kościelnicka <mwk@0x04.net>
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

#include "kernel/qcsat.h"

USING_YOSYS_NAMESPACE

std::vector<int> QuickConeSat::importSig(SigSpec sig)
{
	sig = modwalker.sigmap(sig);
	for (auto bit : sig)
		bits_queue.insert(bit);
	return satgen.importSigSpec(sig);
}

int QuickConeSat::importSigBit(SigBit bit)
{
	bit = modwalker.sigmap(bit);
	bits_queue.insert(bit);
	return satgen.importSigBit(bit);
}

void QuickConeSat::prepare()
{
	int iter_no=0;
	while (!bits_queue.empty())
	{
		pool<ModWalker::PortBit> portbits;
		modwalker.get_drivers(portbits, bits_queue);

		for (auto bit : bits_queue)
			if (bit.wire && bit.wire->get_bool_attribute(ID::onehot) && !imported_onehot.count(bit.wire))
			{
				std::vector<int> bits = satgen.importSigSpec(bit.wire);
				for (int i : bits)
				for (int j : bits)
					if (i != j)
						ez->assume(ez->NOT(i), j);
				imported_onehot.insert(bit.wire);
			}

		bits_queue.clear();

		for (auto &pbit : portbits)
		{
			if (imported_cells.count(pbit.cell))
				continue;
			if (cell_complexity(pbit.cell) > max_cell_complexity)
				continue;
			if (max_cell_outs && GetSize(modwalker.cell_outputs[pbit.cell]) > max_cell_outs)
				continue;
			auto &inputs = modwalker.cell_inputs[pbit.cell];
			bits_queue.insert(inputs.begin(), inputs.end());
			satgen.importCell(pbit.cell);
			imported_cells.insert(pbit.cell);
		}

		if (max_cell_count && GetSize(imported_cells) > max_cell_count)
			break;
		
		/*EDA-2101/(Thierry): Here, we simply importing cells in the "SAT solver" having ports connected
		  to the original "bits_queue", which is actually the direct logic at the DFF output and DFF input.
		  Continuing "while" loop would mean that continuesly adding the cells, also that are connected to these 
		  previously added cells, which results in growing logic cone. This growing logic cone results in large
		  run time for opt_dff -sat command for designs like rsnoc taking 5h30mn and bch_decoder taking 72m8sec.
		  So, we are adding here a "break;" right after 2 iterations for "while loop" to avoid the growing logic cone. 
		  we are doing these changes to drastically reduce runtime taken by the “qcsat” solver for “rsnoc” from 5h30mn 
		  downto 3h08mn with 2 iterations (and 2h45mn with 1 iteration). Here, we kept the iterations to 2 (not 1) 
		  because designs like (EDA-871..*, are sensitive to these iterations, resulting in increased no. of 
		  registers with iterations kept upto 1). 
		  Note: This runtime improvement is also hurting DFF optimization for "design27", resulting in 906 regs 
		  as compare to 815.*/

		iter_no++;
		if (iter_no == 2)
			break;
	}
}

int QuickConeSat::cell_complexity(RTLIL::Cell *cell)
{
	if (cell->type.in(ID($concat), ID($slice), ID($pos), ID($_BUF_)))
		return 0;
	if (cell->type.in(ID($not), ID($and), ID($or), ID($xor), ID($xnor),
			ID($reduce_and), ID($reduce_or), ID($reduce_xor),
			ID($reduce_xnor), ID($reduce_bool),
			ID($logic_not), ID($logic_and), ID($logic_or),
			ID($eq), ID($ne), ID($eqx), ID($nex), ID($fa),
			ID($mux), ID($pmux), ID($bmux), ID($demux), ID($lut), ID($sop),
			ID($_NOT_), ID($_AND_), ID($_NAND_), ID($_OR_), ID($_NOR_),
			ID($_XOR_), ID($_XNOR_), ID($_ANDNOT_), ID($_ORNOT_),
			ID($_MUX_), ID($_NMUX_), ID($_MUX4_), ID($_MUX8_), ID($_MUX16_),
			ID($_AOI3_), ID($_OAI3_), ID($_AOI4_), ID($_OAI4_)))
		return 1;
	if (cell->type.in(ID($neg), ID($add), ID($sub), ID($alu), ID($lcu),
			ID($lt), ID($le), ID($gt), ID($ge)))
		return 2;
	if (cell->type.in(ID($shl), ID($shr), ID($sshl), ID($sshr), ID($shift), ID($shiftx)))
		return 3;
	if (cell->type.in(ID($mul), ID($macc), ID($div), ID($mod), ID($divfloor), ID($modfloor), ID($pow)))
		return 4;
	// Unknown cell.
	return 5;
}
