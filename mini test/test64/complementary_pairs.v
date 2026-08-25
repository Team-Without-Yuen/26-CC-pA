module complementary_pairs(
  a,
  b,
  clk,
  out_and,
  out_a,
  out_q
);
  input a, b, clk;
  output out_and, out_a, out_q;

  wire and_direct;
  wire and_alias;
  wire nand_direct;
  wire and_via_nand;
  wire or_direct;
  wire nor_direct;
  wire not_a;
  wire not_b;
  wire or_demorgan;
  wire xor_direct;
  wire xnor_direct;
  wire q;
  wire not_q;

  and  g_and_direct(and_direct, a, b);
  and  g_and_alias(and_alias, a, b);
  nand g_nand_direct(nand_direct, a, b);
  nand g_and_via_nand(and_via_nand, nand_direct, nand_direct);

  or   g_or_direct(or_direct, a, b);
  nor  g_nor_direct(nor_direct, a, b);
  not  g_not_a(not_a, a);
  not  g_not_b(not_b, b);
  nand g_or_demorgan(or_demorgan, not_a, not_b);

  xor  g_xor_direct(xor_direct, a, b);
  xnor g_xnor_direct(xnor_direct, a, b);

  dff  g_state(.RN(1'b1), .SN(1'b1), .CK(clk), .D(and_direct), .Q(q));
  not  g_not_q(not_q, q);

  buf  g_out_and(out_and, and_direct);
  buf  g_out_a(out_a, a);
  buf  g_out_q(out_q, q);
endmodule
