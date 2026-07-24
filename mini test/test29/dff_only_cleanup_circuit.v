module dff_only_cleanup_circuit(a, b, clk);
input a, b, clk;

wire n_live, n_dead, q;

and g_live(n_live, a, b);
or g_dead(n_dead, a, b);
dff ff0(.D(n_live), .CK(clk), .RN(clk), .SN(clk), .Q(q));

endmodule
