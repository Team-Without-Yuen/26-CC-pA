module functional_pattern_circuit(
    input en,
    input en_a,
    input en_b,
    input rank_a,
    input rank_b,
    input rank_data,
    input data,
    input data_b,
    input clk,
    output q_xor,
    output q_internal,
    output q_ranked,
    output q_safe_reject,
    output q_and,
    output q_plain,
    output q_canonical,
    output q_unreachable
);
    wire xor_delta;
    wire xor_masked;
    wire d_xor;
    wire internal_sel;
    wire internal_delta;
    wire internal_masked;
    wire d_internal;
    wire rank_sel;
    wire rank_delta;
    wire rank_masked;
    wire d_ranked;
    wire safe_not_q;
    wire safe_not_en;
    wire safe_branch_high;
    wire safe_branch_low;
    wire d_safe_reject;
    wire d_and;
    wire not_en;
    wire canonical_hold;
    wire canonical_load;
    wire d_canonical;
    wire constant_zero;
    wire unreachable_dead;
    wire not_q_unreachable;
    wire d_unreachable;

    xor g_xor_delta(xor_delta, q_xor, data);
    and g_xor_masked(xor_masked, en, xor_delta);
    xor g_xor_mux(d_xor, q_xor, xor_masked);
    dff ff_xor(.D(d_xor), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_xor));

    and g_internal_sel(internal_sel, en_a, en_b);
    xor g_internal_delta(internal_delta, q_internal, data_b);
    and g_internal_masked(internal_masked, internal_sel, internal_delta);
    xor g_internal_mux(d_internal, q_internal, internal_masked);
    dff ff_internal(.D(d_internal), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_internal));

    xor g_rank_sel(rank_sel, rank_a, rank_b);
    xor g_rank_delta(rank_delta, q_ranked, rank_data);
    and g_rank_masked(rank_masked, rank_sel, rank_delta);
    xor g_rank_mux(d_ranked, q_ranked, rank_masked);
    dff ff_ranked(.D(d_ranked), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_ranked));

    not g_safe_not_q(safe_not_q, q_safe_reject);
    not g_safe_not_en(safe_not_en, en);
    and g_safe_branch_high(safe_branch_high, en, safe_not_q);
    and g_safe_branch_low(safe_branch_low, safe_not_en, safe_not_q);
    or g_safe_mux(d_safe_reject, safe_branch_high, safe_branch_low);
    dff ff_safe_reject(.D(d_safe_reject), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_safe_reject));

    and g_and_only(d_and, en, data);
    dff ff_and(.D(d_and), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_and));

    dff ff_plain(.D(data_b), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_plain));

    not g_not_en(not_en, en);
    and g_canonical_hold(canonical_hold, not_en, q_canonical);
    and g_canonical_load(canonical_load, en, data);
    or g_canonical_mux(d_canonical, canonical_hold, canonical_load);
    dff ff_canonical(.D(d_canonical), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_canonical));

    xor g_constant_zero(constant_zero, en, en);
    and g_unreachable_dead(unreachable_dead, constant_zero, data);
    not g_not_q_unreachable(not_q_unreachable, q_unreachable);
    xor g_unreachable_mix(d_unreachable, not_q_unreachable, unreachable_dead);
    dff ff_unreachable(.D(d_unreachable), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q_unreachable));
endmodule
