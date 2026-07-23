module sequential_cli_circuit(
    input en,
    input en_n,
    input data,
    input plain,
    input clk,
    output q_high,
    output q_low,
    output q_and,
    output q_plain
);
    wire not_en;
    wire not_en_n;
    wire high_hold;
    wire high_load;
    wire d_high;
    wire low_hold;
    wire low_load;
    wire d_low;
    wire d_and;

    not g_not_en(not_en, en);
    and g_high_hold(high_hold, not_en, q_high);
    and g_high_load(high_load, en, data);
    or g_high_mux(d_high, high_hold, high_load);
    dff ff_high(.D(d_high), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_high));

    not g_not_en_n(not_en_n, en_n);
    and g_low_hold(low_hold, en_n, q_low);
    and g_low_load(low_load, not_en_n, data);
    or g_low_mux(d_low, low_hold, low_load);
    dff ff_low(.D(d_low), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_low));

    and g_and_candidate(d_and, en, data);
    dff ff_and(.D(d_and), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_and));

    dff ff_plain(.D(plain), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_plain));
endmodule
