module function_search_circuit(
  a, b, c, d,
  y_match, y_none,
  y_buf, y_not, y_and, y_nand, y_or, y_nor, y_xor, y_xnor
);
  input a, b, c, d;
  output y_match, y_none;
  output y_buf, y_not, y_and, y_nand, y_or, y_nor, y_xor, y_xnor;
  wire not_a, not_b, not_a_copy, not_b_copy;
  wire a_i, b_i;

  not g_not_a(not_a, a);
  not g_not_b(not_b, b);
  buf g_copy_a(not_a_copy, not_a);
  buf g_copy_b(not_b_copy, not_b);
  or g_match(y_match, a, b);
  xor g_none(y_none, c, d);
  buf g_a_i(a_i, a);
  buf g_b_i(b_i, b);
  buf g_buf(y_buf, a_i);
  not g_not(y_not, a_i);
  and g_and(y_and, a_i, b_i);
  nand g_nand(y_nand, a_i, b_i);
  or g_or(y_or, a_i, b_i);
  nor g_nor(y_nor, a_i, b_i);
  xor g_xor(y_xor, a_i, b_i);
  xnor g_xnor(y_xnor, a_i, b_i);
endmodule
