/*
 * codecs.h - minimal stand-in for Rockbox's apps/codecs.h codec-plugin API.
 * The vendored decoder never calls into the real Rockbox codec_api (ci->...)
 * or plugin framework -- see decoders/ReadMe.md for the audit that
 * confirmed this -- so this header only needs to make platform.h's
 * definitions reachable through the same #include "codecs.h" vendored
 * files use. Not part of upstream Rockbox.
 */
#ifndef MINIAMP3_WMA_CODECS_H
#define MINIAMP3_WMA_CODECS_H

#include "platform.h"

#endif /* MINIAMP3_WMA_CODECS_H */
