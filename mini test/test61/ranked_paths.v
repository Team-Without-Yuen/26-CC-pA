module ranked_paths(a, b, clk, rst_n, y, z, w, tied_out, b_out, q);
    input a;
    input b;
    input clk;
    input rst_n;
    output y;
    output z;
    output w;
    output tied_out;
    output b_out;
    output q;

    wire n_alpha;
    wire n_beta;

    buf g_short(y, a);
    and g_tied(tied_out, a, a);
    buf g_alpha(n_alpha, a);
    buf g_beta(n_beta, a);
    or  g_join(z, n_alpha, n_beta);
    buf g_after(w, z);
    buf g_b_short(b_out, b);

    dff ff(.D(w), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q));
endmodule
