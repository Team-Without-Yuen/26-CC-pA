module constant_signals(
  a,
  b,
  clk,
  out_zero,
  out_one,
  out_nonconstant,
  out_zero_po,
  out_q
);
  input a, b, clk;
  output out_zero, out_one, out_nonconstant, out_zero_po, out_q;

  wire zero_xor;
  wire one_xnor;
  wire zero_and;
  wire q;

  xor g_zero_xor(zero_xor, a, a);
  xnor g_one_xnor(one_xnor, b, b);
  and g_zero_and(zero_and, a, 1'b0);

  buf g_out_zero(out_zero, zero_xor);
  buf g_out_one(out_one, one_xnor);
  buf g_out_nonconstant(out_nonconstant, a);
  buf g_out_zero_po(out_zero_po, zero_and);

  dff g_state(.RN(1'b1), .SN(1'b1), .CK(clk), .D(b), .Q(q));
  buf g_out_q(out_q, q);
endmodule
