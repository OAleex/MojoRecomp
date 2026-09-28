// Adapted from ReXGlue SDK v0.10.0 (BSD-3-Clause).
static const AVCodecParser* const parser_list[] = {
#if CONFIG_MPEGAUDIO_PARSER
    &ff_mpegaudio_parser,
#endif
    NULL};
