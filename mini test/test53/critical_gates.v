module critical_gates(a, b, clk, y0, y1, q);
    input a, b, clk;
    output y0, y1, q;
    wire shared, left, right, dff_d, dangling;

    and g_shared(shared, a, b);
    not g_left(left, shared);
    buf g_left_end(y0, left);
    or g_right(right, shared, a);
    buf g_right_end(y1, right);
    buf g_dff_end(dff_d, left);
    xor g_dangling(dangling, a, b);
    dff ff0(.D(dff_d), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q));
endmodule
