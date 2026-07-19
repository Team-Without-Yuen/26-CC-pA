module top(
    reset, hot, a, b,
    p0, p1, p2, p3, p4, p5, p6, p7,
    h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11,
    r0, r1, r2, r3, r4, r5, r6, r7,
    hot0, hot1, hot2, hot3, hot4, hot5,
    hot6, hot7, hot8, hot9, hot10, hot11,
    inv_out, buf_out, z0, z1, z2, z3
);
  input reset, hot, a, b;
  input p0, p1, p2, p3, p4, p5, p6, p7;
  input h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11;
  output r0, r1, r2, r3, r4, r5, r6, r7;
  output hot0, hot1, hot2, hot3, hot4, hot5;
  output hot6, hot7, hot8, hot9, hot10, hot11;
  output inv_out, buf_out, z0, z1, z2, z3;

  wire inv_mid, inv_chain_out;
  wire dup_a0_n, dup_a1_n, dup_b0_n, dup_b1_n;

  and reset_load0(r0, reset, p0);
  and reset_load1(r1, reset, p1);
  and reset_load2(r2, reset, p2);
  and reset_load3(r3, reset, p3);
  and reset_load4(r4, reset, p4);
  and reset_load5(r5, reset, p5);
  and reset_load6(r6, reset, p6);
  and reset_load7(r7, reset, p7);

  or hot_load0(hot0, hot, h0);
  or hot_load1(hot1, hot, h1);
  or hot_load2(hot2, hot, h2);
  or hot_load3(hot3, hot, h3);
  or hot_load4(hot4, hot, h4);
  or hot_load5(hot5, hot, h5);
  or hot_load6(hot6, hot, h6);
  or hot_load7(hot7, hot, h7);
  or hot_load8(hot8, hot, h8);
  or hot_load9(hot9, hot, h9);
  or hot_load10(hot10, hot, h10);
  or hot_load11(hot11, hot, h11);

  not inv0(inv_mid, a);
  not inv1(inv_chain_out, inv_mid);
  and inv_use(inv_out, inv_chain_out, p4);
  buf keep_buf(buf_out, b);

  not dup_a0(dup_a0_n, a);
  not dup_a1(dup_a1_n, a);
  not dup_b0(dup_b0_n, b);
  not dup_b1(dup_b1_n, b);
  and use_dup_a0(z0, dup_a0_n, p0);
  and use_dup_a1(z1, dup_a1_n, p1);
  or use_dup_b0(z2, dup_b0_n, p2);
  or use_dup_b1(z3, dup_b1_n, p3);
endmodule
