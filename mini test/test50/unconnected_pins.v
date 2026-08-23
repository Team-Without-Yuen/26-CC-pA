module unconnected_pins (
    input a,
    input b,
    input d,
    input clk,
    output y_ok,
    output y_input_hole
);

wire q_internal;

and g_missing_input(y_input_hole, a, );
or g_missing_output(, a, b);
buf g_ok(y_ok, a);
dff ff_open_rn(.D(d), .CK(clk), .RN(), .SN(1'b1), .Q(q_internal));

endmodule
