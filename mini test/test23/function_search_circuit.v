module function_search_circuit(a, b, c, d, y_match, y_none);
  input a, b, c, d;
  output y_match, y_none;
  wire not_a, not_b, not_a_copy, not_b_copy;

  not g_not_a(not_a, a);
  not g_not_b(not_b, b);
  buf g_copy_a(not_a_copy, not_a);
  buf g_copy_b(not_b_copy, not_b);
  or g_match(y_match, a, b);
  xor g_none(y_none, c, d);
endmodule
