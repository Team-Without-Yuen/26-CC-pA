module conditional_equivalence_circuit(a, b, en, y_mux, y_a, y_b);
  input a, b, en;
  output y_mux, y_a, y_b;

  wire not_en;
  wire select_a;
  wire select_b;

  not g_not_en(not_en, en);
  and g_select_a(select_a, a, not_en);
  and g_select_b(select_b, b, en);
  or g_mux(y_mux, select_a, select_b);
  buf g_a(y_a, a);
  buf g_b(y_b, b);
endmodule
