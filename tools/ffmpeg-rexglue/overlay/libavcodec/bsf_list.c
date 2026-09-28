// Adapted from ReXGlue SDK v0.10.0 (BSD-3-Clause).
static const AVBitStreamFilter* const bitstream_filters[] = {
#if CONFIG_NULL_BSF
    &ff_null_bsf,
#endif
    NULL};
