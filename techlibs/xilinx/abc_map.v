/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *                2019  Eddie Hung    <eddie@fpgeh.com>
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

// ============================================================================

module FDRE (output reg Q, input C, CE, D, R);
  parameter [0:0] INIT = 1'b0;
  parameter [0:0] IS_C_INVERTED = 1'b0;
  parameter [0:0] IS_D_INVERTED = 1'b0;
  parameter [0:0] IS_R_INVERTED = 1'b0;
  wire \$nextQ ;
  \$__ABC_FDRE #(
    .INIT(INIT),
    .IS_C_INVERTED(IS_C_INVERTED),
    .IS_D_INVERTED(IS_D_INVERTED),
    .IS_R_INVERTED(IS_R_INVERTED),
    .CLK_POLARITY(!IS_C_INVERTED),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .R(R)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(Q));
endmodule
module FDRE_1 (output reg Q, input C, CE, D, R);
  parameter [0:0] INIT = 1'b0;
  wire \$nextQ ;
  \$__ABC_FDRE_1 #(
      .INIT(|0),
    .CLK_POLARITY(1'b0),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .R(R)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(Q));
endmodule

module FDCE (output reg Q, input C, CE, D, CLR);
  parameter [0:0] INIT = 1'b0;
  parameter [0:0] IS_C_INVERTED = 1'b0;
  parameter [0:0] IS_D_INVERTED = 1'b0;
  parameter [0:0] IS_CLR_INVERTED = 1'b0;
  wire \$nextQ , \$currQ ;
  \$__ABC_FDCE #(
    .INIT(INIT),
    .IS_C_INVERTED(IS_C_INVERTED),
    .IS_D_INVERTED(IS_D_INVERTED),
    .IS_CLR_INVERTED(IS_CLR_INVERTED),
    .CLK_POLARITY(!IS_C_INVERTED),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .CLR(CLR)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(\$currQ ));
  \$__ABC_ASYNC abc_async (.A(\$currQ ), .S(CLR), .Y(Q));
endmodule
module FDCE_1 (output reg Q, input C, CE, D, CLR);
  parameter [0:0] INIT = 1'b0;
  wire \$nextQ , \$currQ ;
  \$__ABC_FDCE_1 #(
    .INIT(INIT),
    .CLK_POLARITY(1'b0),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .CLR(CLR)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(\$currQ ));
  \$__ABC_ASYNC abc_async (.A(\$currQ ), .S(CLR), .Y(Q));
endmodule

module FDPE (output reg Q, input C, CE, D, PRE);
  parameter [0:0] INIT = 1'b0;
  parameter [0:0] IS_C_INVERTED = 1'b0;
  parameter [0:0] IS_D_INVERTED = 1'b0;
  parameter [0:0] IS_PRE_INVERTED = 1'b0;
  wire \$nextQ , \$currQ ;
  \$__ABC_FDPE #(
    .INIT(INIT),
    .IS_C_INVERTED(IS_C_INVERTED),
    .IS_D_INVERTED(IS_D_INVERTED),
    .IS_PRE_INVERTED(IS_PRE_INVERTED),
    .CLK_POLARITY(!IS_C_INVERTED),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .PRE(PRE)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(\$currQ ));
  \$__ABC_ASYNC abc_async (.A(\$currQ ), .S(PRE), .Y(Q));
endmodule
module FDPE_1 (output reg Q, input C, CE, D, PRE);
  parameter [0:0] INIT = 1'b0;
  wire \$nextQ , \$currQ ;
  \$__ABC_FDPE_1 #(
    .INIT(INIT),
    .CLK_POLARITY(1'b0),
    .EN_POLARITY(1'b1)
  ) _TECHMAP_REPLACE_ (
    .D(D), .Q(\$nextQ ), .\$pastQ (Q), .C(C), .CE(CE), .PRE(PRE)
  );
  \$__ABC_FF_ abc_dff (.D(\$nextQ ), .Q(\$currQ ));
  \$__ABC_ASYNC abc_async (.A(\$currQ ), .S(PRE), .Y(Q));
endmodule

module RAM32X1D (
  output DPO, SPO,
  input  D,
  input  WCLK,
  input  WE,
  input  A0, A1, A2, A3, A4,
  input  DPRA0, DPRA1, DPRA2, DPRA3, DPRA4
);
  parameter INIT = 32'h0;
  parameter IS_WCLK_INVERTED = 1'b0;
  wire \$DPO , \$SPO ;
  RAM32X1D #(
    .INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)
  ) _TECHMAP_REPLACE_ (
    .DPO(\$DPO ), .SPO(\$SPO ),
    .D(D), .WCLK(WCLK), .WE(WE),
    .A0(A0), .A1(A1), .A2(A2), .A3(A3), .A4(A4),
    .DPRA0(DPRA0), .DPRA1(DPRA1), .DPRA2(DPRA2), .DPRA3(DPRA3), .DPRA4(DPRA4)
  );
  \$__ABC_LUT6 dpo (.A(\$DPO ), .S({1'b0, A0, A1, A2, A3, A4}), .Y(DPO));
  \$__ABC_LUT6 spo (.A(\$SPO ), .S({1'b0, A0, A1, A2, A3, A4}), .Y(SPO));
endmodule

module RAM64X1D (
  output DPO, SPO,
  input  D,
  input  WCLK,
  input  WE,
  input  A0, A1, A2, A3, A4, A5,
  input  DPRA0, DPRA1, DPRA2, DPRA3, DPRA4, DPRA5
);
  parameter INIT = 64'h0;
  parameter IS_WCLK_INVERTED = 1'b0;
  wire \$DPO , \$SPO ;
  RAM64X1D #(
    .INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)
  ) _TECHMAP_REPLACE_ (
    .DPO(\$DPO ), .SPO(\$SPO ),
    .D(D), .WCLK(WCLK), .WE(WE),
    .A0(A0), .A1(A1), .A2(A2), .A3(A3), .A4(A4), .A5(A5),
    .DPRA0(DPRA0), .DPRA1(DPRA1), .DPRA2(DPRA2), .DPRA3(DPRA3), .DPRA4(DPRA4), .DPRA5(DPRA5)
  );
  \$__ABC_LUT6 dpo (.A(\$DPO ), .S({A0, A1, A2, A3, A4, A5}), .Y(DPO));
  \$__ABC_LUT6 spo (.A(\$SPO ), .S({A0, A1, A2, A3, A4, A5}), .Y(SPO));
endmodule

module RAM128X1D (
  output       DPO, SPO,
  input        D,
  input        WCLK,
  input        WE,
  input  [6:0] A, DPRA
);
  parameter INIT = 128'h0;
  parameter IS_WCLK_INVERTED = 1'b0;
  wire \$DPO , \$SPO ;
  RAM128X1D #(
    .INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)
  ) _TECHMAP_REPLACE_ (
    .DPO(\$DPO ), .SPO(\$SPO ),
    .D(D), .WCLK(WCLK), .WE(WE),
    .A(A),
    .DPRA(DPRA)
  );
  \$__ABC_LUT7 dpo (.A(\$DPO ), .S(A), .Y(DPO));
  \$__ABC_LUT7 spo (.A(\$SPO ), .S(A), .Y(SPO));
endmodule

module SRL16E (
  output Q,
  input A0, A1, A2, A3, CE, CLK, D
);
  parameter [15:0] INIT = 16'h0000;
  parameter [0:0] IS_CLK_INVERTED = 1'b0;
  wire \$Q ;
  SRL16E #(
    .INIT(INIT), .IS_CLK_INVERTED(IS_CLK_INVERTED)
  ) _TECHMAP_REPLACE_ (
    .Q(\$Q ),
    .A0(A0), .A1(A1), .A2(A2), .A3(A3), .CE(CE), .CLK(CLK), .D(D)
  );
  \$__ABC_LUT6 q (.A(\$Q ), .S({1'b1, A0, A1, A2, A3, 1'b1}), .Y(Q));
endmodule

module SRLC32E (
  output Q,
  output Q31,
  input [4:0] A,
  input CE, CLK, D
);
  parameter [31:0] INIT = 32'h00000000;
  parameter [0:0] IS_CLK_INVERTED = 1'b0;
  wire \$Q ;
  SRLC32E #(
    .INIT(INIT), .IS_CLK_INVERTED(IS_CLK_INVERTED)
  ) _TECHMAP_REPLACE_ (
    .Q(\$Q ), .Q31(Q31),
    .A(A), .CE(CE), .CLK(CLK), .D(D)
  );
  \$__ABC_LUT6 q (.A(\$Q ), .S({1'b1, A}), .Y(Q));
endmodule
