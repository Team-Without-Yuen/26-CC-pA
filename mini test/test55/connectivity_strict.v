module connectivity_strict (
    input a,
    input b,
    input clk,
    input rst_n,
    input [3:0] data_in,
    input [0:2] ascending_in,
    output y,
    output q,
    output [1:0] data_out
);
    wire n1;
    wire n2;
    wire n3;
    wire n_bus;
    wire n_ascending;

    and g_and0 (n1, a, b);
    buf g_buf0 (n2, n1);
    or  g_or0  (n3, n1, a);
    xor g_xor0 (y, n2, n3);
    dff g_dff0 (.RN(rst_n), .SN(1'b1), .CK(clk), .D(n1), .Q(q));
    and g_bus_multi (n_bus, data_in[0], data_in[2]);
    buf g_bus_ascending (n_ascending, ascending_in[2]);
    buf g_bus_output (data_out[1], a);
endmodule
