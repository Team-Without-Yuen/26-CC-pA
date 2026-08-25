module tied_input_path (
    input a,
    output y
);
    wire tied_path;
    wire distinct_path;

    and g_tied(tied_path, a, a);
    buf g_distinct(distinct_path, a);
    or g_merge(y, tied_path, distinct_path);
endmodule
