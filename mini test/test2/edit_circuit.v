module edit_circuit(a, b, c, d, e, y, keep, aux, same_y, const_y);
input a, b, c, d, e;
output y, keep, aux, same_y, const_y;

wire and0_out, and1_out, or_out;
wire inv0_out, inv1_out, buf_mid;
wire cd0_out, cd1_out, live_mix, aux_buf_mid;
wire same_out, const_mid;
wire dead_mid, dead_out, dead2_mid, dead2_out, floating_buf_out;

and g_live_and0(and0_out, a, b);
and g_live_and1_dup(and1_out, a, b);
or g_live_or(or_out, and0_out, and1_out);

not g_inv0(inv0_out, or_out);
not g_inv1(inv1_out, inv0_out);
buf g_buf0(buf_mid, inv1_out);
buf g_buf1(y, buf_mid);

and g_live_cd0(cd0_out, c, d);
and g_live_cd1_dup(cd1_out, c, d);
xor g_live_mix(live_mix, cd0_out, cd1_out);
buf g_aux_buf(aux_buf_mid, live_mix);
or g_aux(aux, aux_buf_mid, e);

or g_same_or(same_out, d, d);
buf g_same_buf(same_y, same_out);

and g_const_and(const_mid, e, 1'b1);
buf g_const_buf(const_y, const_mid);

and g_dead_and(dead_mid, a, c);
not g_dead_not(dead_out, dead_mid);
nand g_dead_nand(dead2_mid, b, d);
nor g_dead_nor(dead2_out, dead2_mid, e);
buf g_floating_buf(floating_buf_out, dead2_out);

buf g_keep(keep, c);

endmodule
