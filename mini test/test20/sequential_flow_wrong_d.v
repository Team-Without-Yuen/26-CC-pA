module sequential_flow_circuit(
    input en,
    input en_n,
    input data,
    input clk,
    output q_high,
    output q_low,
    output q_and,
    output aux,
    output const_out
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
    wire aux_mid;

    not g_not_en(not_en, en);
    not g_not_en_n(not_en_n, en_n);
    and g_high_hold(high_hold, not_en, q_high);
    and g_high_load(high_load, en, data);
    or g_high_mux(d_high, high_hold, high_load);

    // Only this D connection differs from the original design.
    dff ff_high(.D(data), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_high));

    and g_low_hold(low_hold, en_n, q_low);
    and g_low_load(low_load, not_en_n, data);
    or g_low_mux(d_low, low_hold, low_load);
    dff ff_low(.D(d_low), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_low));

    and g_and_candidate(d_and, en, data);
    dff ff_and(.D(d_and), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_and));

    not g_aux_first(aux_mid, data);
    not g_aux_second(aux, aux_mid);
    nand g_const_nand(const_out, data, 1'b1);
endmodule
