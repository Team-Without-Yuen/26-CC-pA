module path_depth_predicate(shared, a, b, clk, rst_n, y, tied_out, q);
    input shared;
    input a;
    input b;
    input clk;
    input rst_n;
    output shared;
    output y;
    output tied_out;
    output q;

    wire p1;
    wire p2;
    wire p3;
    wire m1;
    wire m2;

    buf g_p1(p1, a);
    buf g_p2(p2, p1);
    buf g_p3(p3, p2);
    or  g_m1(m1, a, p1);
    or  g_m2(m2, m1, p2);
    or  g_y(y, m2, p3);

    // Same source net on two pins is one structural path edge.
    and g_tied(tied_out, a, a);

    dff ff(.D(y), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q));
endmodule
