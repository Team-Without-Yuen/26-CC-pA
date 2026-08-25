module fanout_ranking (
    input high,
    input tie_high,
    input mid,
    input low,
    input zero,
    input clk,
    input rst_n,
    output y0,
    output y1,
    output q
);
    wire n_high_a;
    wire n_high_b;
    wire n_tie_a;
    wire n_tie_b;
    wire n_mid;
    wire n_low;

    and g_high_pair (n_high_a, high, high);
    buf g_high_single (n_high_b, high);
    and g_tie_pair (n_tie_a, tie_high, tie_high);
    buf g_tie_single (n_tie_b, tie_high);
    and g_mid (n_mid, mid, mid);
    buf g_low (n_low, low);

    buf g_y0 (y0, n_high_a);
    buf g_y1 (y1, n_mid);
    dff g_q (.RN(rst_n), .SN(1'b1), .CK(clk), .D(n_low), .Q(q));
endmodule
