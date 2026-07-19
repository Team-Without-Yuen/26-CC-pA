module function_search_cli_circuit(a, b, c, d, target, no_match);
  input a, b, c, d;
  output target, no_match;

  wire not_a, not_b;
  wire not_a_copy, not_b_copy;
  wire and_ab, nand_ab, nor_ab, xor_ab, xnor_ab;

  not g_not_a(not_a, a);
  not g_not_b(not_b, b);
  buf g_copy_a(not_a_copy, not_a);
  buf g_copy_b(not_b_copy, not_b);

  and g_and(and_ab, a, b);
  nand g_nand(nand_ab, a, b);
  nor g_nor(nor_ab, a, b);
  xor g_xor(xor_ab, a, b);
  xnor g_xnor(xnor_ab, a, b);

  or g_target(target, a, b);
  xor g_no_match(no_match, c, d);
endmodule
