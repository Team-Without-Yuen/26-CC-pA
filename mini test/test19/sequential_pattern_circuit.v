module sequential_pattern_circuit(
    input en,
    input en_n,
    input data,
    input clk,
    output q_high,
    output q_low,
    output q_nand,
    output q_nand_inv,
    output q_nor,
    output q_pos,
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
    wire nand_hold;
    wire nand_load;
    wire d_nand;
    wire not_data;
    wire nand_inv_hold;
    wire nand_inv_load;
    wire d_nand_inv;
    wire nor_hold;
    wire nor_load;
    wire d_nor;
    wire pos_hold;
    wire pos_load;
    wire d_pos;
    wire d_and;

    not g_not_en(not_en, en);
    not g_not_en_n(not_en_n, en_n);

    and g_high_hold(high_hold, not_en, q_high);
    and g_high_load(high_load, en, data);
    or g_high_mux(d_high, high_hold, high_load);
    dff ff_high(.D(d_high), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_high));

    and g_low_hold(low_hold, en_n, q_low);
    and g_low_load(low_load, not_en_n, data);
    or g_low_mux(d_low, low_hold, low_load);
    dff ff_low(.D(d_low), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_low));

    nand g_nand_hold(nand_hold, not_en, q_nand);
    nand g_nand_load(nand_load, en, data);
    nand g_nand_mux(d_nand, nand_hold, nand_load);
    dff ff_nand(.D(d_nand), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_nand));

    not g_not_data(not_data, data);
    nand g_nand_inv_hold(nand_inv_hold, not_en, q_nand_inv);
    nand g_nand_inv_load(nand_inv_load, en, not_data);
    nand g_nand_inv_mux(d_nand_inv, nand_inv_hold, nand_inv_load);
    dff ff_nand_inv(.D(d_nand_inv), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_nand_inv));

    nor g_nor_hold(nor_hold, en, q_nor);
    nor g_nor_load(nor_load, not_en, data);
    nor g_nor_mux(d_nor, nor_hold, nor_load);
    dff ff_nor(.D(d_nor), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_nor));

    or g_pos_hold(pos_hold, en, q_pos);
    or g_pos_load(pos_load, not_en, data);
    and g_pos_mux(d_pos, pos_hold, pos_load);
    dff ff_pos(.D(d_pos), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_pos));

    and g_and_candidate(d_and, en, data);
    dff ff_and(.D(d_and), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_and));

    dff ff_plain(.D(data), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_plain));
endmodule
