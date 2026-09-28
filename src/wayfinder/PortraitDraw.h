#pragma once
#include "wayfinder/EntityPortraits.h"

namespace wayfinder {
// A portrait is the submission boundary: all its parts use one source texture,
// and the batch is flushed before another species can bind a different source.
template <class DrawPart, class Flush>
void submitPortrait(EntityPortrait const& portrait, DrawPart&& drawPart, Flush&& flush) {
    for (auto const& part : portrait.parts) drawPart(portrait.texture, part);
    flush();
}
} // namespace wayfinder
