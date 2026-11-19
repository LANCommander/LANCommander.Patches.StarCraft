// Palette lookup and scaling of the game's 8-bit frame buffer.

Texture2D<uint> g_indices : register(t0);   // frame, one palette index per texel
Texture2D<float4> g_palette : register(t1); // 256 x 1
Texture2D<float4> g_overlay : register(t2); // GDI dialog layer in frame space; unbound or alpha 0 = frame shows
Texture2D<uint> g_map : register(t3);       // map frame shown through the frame's key pixels (zoom)

cbuffer Constants : register(b0) {
  float2 g_srcSize;    // frame size in texels
  float2 g_dstOrigin;  // top-left of the scaled image in render-target pixels
  float2 g_dstSize;    // size of the scaled image in render-target pixels
  uint g_filter;       // 0 = nearest, 1 = sharp bilinear
  uint g_pad;
  float2 g_mapSize;    // map frame size in texels
  float2 g_mapOrigin;  // area of the frame the map fills, in frame texels
  float2 g_mapExtent;
  uint g_key;          // frame palette index that shows the map
  uint g_mapOn;
  float4 g_mapSource;  // shown part of the map frame: x, y (may be fractional), w, h in map texels
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  // One triangle covering the viewport.
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 MapTexel(int2 p) {
  p = clamp(p, int2(0, 0), int2(g_mapSize) - 1);
  return g_palette.Load(int3(g_map.Load(int3(p, 0)), 0, 0));
}

// Sharp bilinear: nearest-neighbour inside each texel, with a one-output-pixel blend at texel edges so
// non-integer scale factors don't produce uneven pixel widths. `scale` = output pixels per texel; returns the
// top-left texel of the 2x2 footprint and the blend weights.
int2 SharpFootprint(float2 t, float2 scale, out float2 f) {
  float2 c = t - 0.5;
  f = saturate((frac(c) - 0.5) * scale + 0.5);
  return int2(floor(c));
}

// The map at frame position t (in frame texels), sampled at the map's own resolution.
float4 MapAt(float2 t) {
  float2 tm = g_mapSource.xy + (t - g_mapOrigin) / g_mapExtent * g_mapSource.zw;
  float2 f;
  int2 b = SharpFootprint(tm, g_dstSize / g_srcSize * g_mapExtent / g_mapSource.zw, f);
  if (g_filter == 0) {  // nearest
    b = int2(floor(tm));
    f = float2(0, 0);
  }
  float4 top = lerp(MapTexel(b), MapTexel(b + int2(1, 0)), f.x);
  float4 bottom = lerp(MapTexel(b + int2(0, 1)), MapTexel(b + int2(1, 1)), f.x);
  return lerp(top, bottom, f.y);
}

bool ShowsMap(int2 p, uint index) {
  return g_mapOn != 0 && index == g_key && all(float2(p) >= g_mapOrigin) && all(float2(p) < g_mapOrigin + g_mapExtent);
}

float4 Texel(int2 p) {
  p = clamp(p, int2(0, 0), int2(g_srcSize) - 1);
  // Unbound or out-of-range loads return 0, so with no overlay every texel comes from the palette.
  float4 o = g_overlay.Load(int3(p, 0));
  if (o.a > 0.5) return float4(o.rgb, 1);
  uint index = g_indices.Load(int3(p, 0));
  if (ShowsMap(p, index)) return MapAt(float2(p) + 0.5);  // UI edge blends against the map beneath
  return g_palette.Load(int3(index, 0, 0));
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
  float2 t = (pos.xy - g_dstOrigin) / g_dstSize * g_srcSize;
  if (g_mapOn != 0) {
    int2 p = clamp(int2(floor(t)), int2(0, 0), int2(g_srcSize) - 1);
    if (g_overlay.Load(int3(p, 0)).a <= 0.5 && ShowsMap(p, g_indices.Load(int3(p, 0)))) return MapAt(t);
  }
  if (g_filter == 0) return Texel(int2(floor(t)));
  float2 f;
  int2 b = SharpFootprint(t, g_dstSize / g_srcSize, f);
  float4 top = lerp(Texel(b), Texel(b + int2(1, 0)), f.x);
  float4 bottom = lerp(Texel(b + int2(0, 1)), Texel(b + int2(1, 1)), f.x);
  return lerp(top, bottom, f.y);
}
