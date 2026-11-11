// e4 - per-sprite neon glow.
//
// glow/make_glow.py bakes a glow around selected sprites (enemies, explosions,
// the title) into their replacement PNGs, on a canvas enlarged by (scale_x,
// scale_y), and lists them in <texture_dir>/replace/glow/glow_scale.txt. When
// the game draws one of those textures, e4 enlarges the sprite's quad by the
// same factors around the shape's centre, so the shape keeps its size and the
// glow spills out around it.
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc.h>

namespace e4::glow {

// From the texture loader hook: name, guest size and replacement hash.
void OnTextureLoaded(uint32_t texture, const std::string& name, uint32_t width, uint32_t height,
                     uint64_t hash);

// Around a sub_82343E38 (draw sprite) call: r5 texture, r6 -> position[2],
// r8 -> pivot[2] (null = centred), r9 -> scale[2].
struct SpriteScaleUndo {
  uint32_t pos_addr = 0, scale_addr = 0;
  uint32_t pos[2], scale[2];
};
bool BeginSprite(PPCContext& ctx, uint8_t* base, SpriteScaleUndo& undo);
void EndSprite(uint8_t* base, const SpriteScaleUndo& undo);

}  // namespace e4::glow
