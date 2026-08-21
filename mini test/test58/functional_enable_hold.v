module functional_enable_hold(
    input en,
    input en_a,
    input en_b,
    input data,
    input clk,
    output q_canonical,
    output q_xor,
    output q_derived,
    output q_nand,
    output q_nor,
    output q_data_gate,
    output q_hold,
    output q_toggle,
    output q_cross
);
    wire not_en;
    wire canonical_hold;
    wire canonical_load;
    wire d_canonical;
    wire xor_delta;
    wire xor_masked;
    wire d_xor;
    wire not_en_b;
    wire inner_hold;
    wire inner_load;
    wire inner_mux;
    wire not_en_a;
    wire outer_hold;
    wire outer_load;
    wire d_derived;
    wire nand_load;
    wire nand_hold;
    wire d_nand;
    wire not_data;
    wire not_q_nor;
    wire nor_load;
    wire nor_hold;
    wire nor_join;
    wire d_nor;
    wire d_data_gate;
    wire d_toggle;
    wire cross_hold;
    wire cross_load;
    wire d_cross;

    not g_not_en(not_en, en);
    and g_canonical_hold(canonical_hold, not_en, q_canonical);
    and g_canonical_load(canonical_load, en, data);
    or g_canonical_mux(d_canonical, canonical_hold, canonical_load);
    dff ff_canonical(.D(d_canonical), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_canonical));

    xor g_xor_delta(xor_delta, q_xor, data);
    and g_xor_masked(xor_masked, en, xor_delta);
    xor g_xor_mux(d_xor, q_xor, xor_masked);
    dff ff_xor(.D(d_xor), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_xor));

    not g_not_en_b(not_en_b, en_b);
    and g_inner_hold(inner_hold, not_en_b, q_derived);
    and g_inner_load(inner_load, en_b, data);
    or g_inner_mux(inner_mux, inner_hold, inner_load);
    not g_not_en_a(not_en_a, en_a);
    and g_outer_hold(outer_hold, not_en_a, q_derived);
    and g_outer_load(outer_load, en_a, inner_mux);
    or g_outer_mux(d_derived, outer_hold, outer_load);
    dff ff_derived(.D(d_derived), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_derived));

    nand g_nand_load(nand_load, en, data);
    nand g_nand_hold(nand_hold, not_en, q_nand);
    nand g_nand_mux(d_nand, nand_load, nand_hold);
    dff ff_nand(.D(d_nand), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_nand));

    not g_not_data(not_data, data);
    not g_not_q_nor(not_q_nor, q_nor);
    nor g_nor_load(nor_load, not_en, not_data);
    nor g_nor_hold(nor_hold, en, not_q_nor);
    nor g_nor_join(nor_join, nor_load, nor_hold);
    not g_nor_mux(d_nor, nor_join);
    dff ff_nor(.D(d_nor), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_nor));

    and g_data_gate(d_data_gate, en, data);
    dff ff_data_gate(.D(d_data_gate), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_data_gate));

    dff ff_hold(.D(q_hold), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_hold));

    not g_toggle(d_toggle, q_toggle);
    dff ff_toggle(.D(d_toggle), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_toggle));

    and g_cross_hold(cross_hold, not_en, q_canonical);
    and g_cross_load(cross_load, en, data);
    or g_cross_mux(d_cross, cross_hold, cross_load);
    dff ff_cross(.D(d_cross), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_cross));
endmodule
