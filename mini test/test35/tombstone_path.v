module tombstone_path (
    input a,
    input b,
    output y
);
    wire live_mid;
    wire dead_mid;
    wire dead_out;

    buf g_live(live_mid, a);
    buf g_output(y, live_mid);

    buf g_dead_mid(dead_mid, a);
    buf g_dead_out(dead_out, dead_mid);
endmodule
