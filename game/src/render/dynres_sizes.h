#pragma once
// DE-362: the internal render sizes of the dynres levels, set by the warm-up in NpcRender (first dynres frame). Passes that create their targets lazily at first use
// (ground live G-buffer, bloom) make the targets of every level once when they start, so a scale change at run time allocates nothing.
inline int g_warm_n = 0, g_warm_w[8] = {}, g_warm_h[8] = {};
