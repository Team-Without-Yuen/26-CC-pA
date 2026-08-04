module canonical_wrapper_probe (
    input en,
    input data,
    input clk,
    output q_wrap,
    output q_nor_inv,
    output q_wrong
);
    wire not_en_nand;
    wire data_inv;
    wire data_double;
    wire wrap_hold_raw;
    wire wrap_hold_inv;
    wire wrap_hold_double;
    wire wrap_load_raw;
    wire wrap_load_buf;
    wire wrap_mux_raw;
    wire d_wrap;

    nand g_not_en_nand (not_en_nand, en, en);
    not g_data_inv (data_inv, data);
    not g_data_double (data_double, data_inv);
    and g_wrap_hold_raw (wrap_hold_raw, not_en_nand, q_wrap);
    not g_wrap_hold_inv (wrap_hold_inv, wrap_hold_raw);
    not g_wrap_hold_double (wrap_hold_double, wrap_hold_inv);
    and g_wrap_load_raw (wrap_load_raw, en, data_double);
    buf g_wrap_load_buf (wrap_load_buf, wrap_load_raw);
    or g_wrap_mux_raw (wrap_mux_raw, wrap_hold_double, wrap_load_buf);
    buf g_wrap_root_buf (d_wrap, wrap_mux_raw);
    dff ff_wrap (.Q(q_wrap), .D(d_wrap), .CK(clk));

    wire not_en_nor;
    wire nor_hold;
    wire nor_load;
    wire d_nor_inv;

    nor g_not_en_nor (not_en_nor, en, en);
    and g_nor_inv_hold (nor_hold, not_en_nor, q_nor_inv);
    and g_nor_inv_load (nor_load, en, data);
    or g_nor_inv_mux (d_nor_inv, nor_hold, nor_load);
    dff ff_nor_inv (.Q(q_nor_inv), .D(d_nor_inv), .CK(clk));

    wire wrong_hold;
    wire wrong_load;
    wire d_wrong;

    and g_wrong_hold (wrong_hold, not_en_nand, q_wrap);
    and g_wrong_load (wrong_load, en, data);
    or g_wrong_mux (d_wrong, wrong_hold, wrong_load);
    dff ff_wrong (.Q(q_wrong), .D(d_wrong), .CK(clk));
endmodule
