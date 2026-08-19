module path_options (
    input a,
    input b,
    input clk,
    input rst_n,
    output y,
    output q
);
    wire p;
    wire r;
    wire m;
    wire d1;

    // a 經由兩條分支重收斂到 m：a->y 共 2 條路徑，m 是必經點
    buf  g_p  (p, a);
    buf  g_r  (r, a);
    or   g_m  (m, p, r);
    buf  g_y  (y, m);

    // DFF 分支，供 dff_q / dff_d endpoint 使用
    and  g_d  (d1, m, b);
    dff  g_ff (.RN(rst_n), .SN(1'b1), .CK(clk), .D(d1), .Q(q));
endmodule
