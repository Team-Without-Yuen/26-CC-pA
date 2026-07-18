module tools_batch5_circuit(a, b, c, d, clk, direct_a, y, z, short_path, qd);
  input a, b, c, d, clk, direct_a;
  output direct_a, y, z, short_path, qd;
  wire n_shared, n_left, n_right;
  wire n_d1, n_d2, n_d3, d_in;

  and g_shared(n_shared, a, b);
  or g_left(n_left, n_shared, c);
  not g_y(y, n_left);

  xor g_right(n_right, n_shared, d);
  buf g_z(z, n_right);

  buf g_short(short_path, a);

  and g_d1(n_d1, a, c);
  or g_d2(n_d2, n_d1, b);
  xor g_d3(n_d3, n_d2, c);
  nor g_d4(d_in, n_d3, d);
  dff ff0(.RN(1'b1), .SN(1'b1), .CK(clk), .D(d_in), .Q(qd));
endmodule
