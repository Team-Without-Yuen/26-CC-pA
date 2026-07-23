module top(a, b, y, z);
  input a;
  input b;
  output y;
  output z;
  wire n1, n2, net_g3_opt_2;
  and g0(n1, a, b);
  not g1(n2, n1);
  buf g2(y, n2);
  nor g3_opt_2(net_g3_opt_2, a, b);
  nor g3_opt_3(z, net_g3_opt_2, net_g3_opt_2);

endmodule
