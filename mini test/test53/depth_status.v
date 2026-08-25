module depth_status(
    input a,
    output defined_out,
    output undriven_out,
    output floating_fanout_out
);
    wire floating_root;

    buf g_defined(defined_out, a);
    buf g_floating(floating_fanout_out, floating_root);
endmodule
