/* A host-drawn 3D scene placed inside a guest frame (nv2a_gpu_hostscene.inc.c).
 *
 * The game side fills an XboxHostScene and calls xbox_HostSceneSet() on its own thread, then
 * marks where the scene goes in its own frame: a draw with a texture it named through
 * xbox_HostSceneTrigger(), or one NV097_SET_COLOR_CLEAR_VALUE with XBOX_HOSTSCENE_MAGIC. When
 * the pushbuffer executor reaches the mark it draws the latest scene into the colour surface
 * bound at that point, with its own depth, and everything the title draws after it lands on
 * top.
 * Units: metres, y up, right-handed. */
#ifndef XBOX_HOSTSCENE_H
#define XBOX_HOSTSCENE_H
#include <stdint.h>
#include <stddef.h>

#define XBOX_HOSTSCENE_MAGIC  0x48534E31u   /* "HSN1" */
#define XBOX_HOSTSCENE_BOXES  320
#define XBOX_HOSTSCENE_LIGHTS 4
#define XBOX_HOSTSCENE_MESHES 96                  /* placed meshes per scene */
#define XBOX_HOSTSCENE_SHADOWS 16                 /* soft contact shadows on the floor */
#define HS_MESH_TRIPLANAR 0x10                    /* xbox_HostMeshAdd mat flag: texture by world position */
#define HS_MESH_PANORAMA  0x20                    /* ... an equirectangular view, looked up by direction (windows) */

enum { HS_MAT_MATTE, HS_MAT_WOOD, HS_MAT_TILE, HS_MAT_MIRROR, HS_MAT_EMISSIVE, HS_MAT_FABRIC, HS_MAT_GLASS, HS_MAT_SCREEN };

typedef struct { float lo[3], hi[3]; uint32_t rgb; uint32_t mat; } XboxHostBox;
typedef struct { float pos[3], color[3], range; } XboxHostLight;
/* A placed mesh: one registered with xbox_HostMeshAdd, and a row-vector 4x4 (metres) */
typedef struct { int mesh; float m[16]; } XboxHostMeshInst;

typedef struct {
    float eye[3], at[3], fov_y;              /* degrees, vertical */
    float sky[3], ground[3];                 /* hemisphere ambient */
    float time;                              /* seconds, for animated surfaces */
    int highlight;                           /* box index drawn with a soft glow, -1 none */
    int nbox, nlight;
    XboxHostBox box[XBOX_HOSTSCENE_BOXES];
    XboxHostLight light[XBOX_HOSTSCENE_LIGHTS];
    int nmesh;
    XboxHostMeshInst mesh[XBOX_HOSTSCENE_MESHES];
    /* The game camera's depth, so the scene shares the game's depth buffer and the game's own
     * 3D drawn after it is hidden behind it: depth-buffer z = zmap[0] + zmap[1] / distance,
     * distance in game units = metres x zmap[2]; zmap[3] != 0 when set. */
    float zmap[4];
    /* The room's walls (x0, z0, x1, z1) and ceiling height, for the shading of its corners and
     * seams; shadow footprints (centre x, z, half extent x, z) on the floor. */
    float room[4], ceiling;
    int nshadow;
    float shadow[XBOX_HOSTSCENE_SHADOWS][4];
} XboxHostScene;

#ifdef __cplusplus
extern "C" {
#endif
void xbox_HostSceneSet(const XboxHostScene *scene);
/* The trigger: a guest draw whose stage-0 texture is at one of these offsets (low 26 bits)
 * is replaced by the scene. The game side draws a small rect with a texture of its own. */
void xbox_HostSceneTrigger(const uint32_t *offsets, int n);
int  xbox_HostSceneDrawn(void);              /* frames drawn so far (tests and logs) */
/* A textured triangle list (8 floats a vertex: position, normal, uv; this scene's axes) and
 * its RGBA8 base-colour texture, copied; mat (HS_MAT_*) adds that surface's detail and shine
 * on top of the texture. Returns the mesh id, or -1. Any thread. */
int  xbox_HostMeshAdd(const float *pnu, int nverts, const uint32_t *rgba, int tw, int th, int mat);
#ifdef __cplusplus
}
#endif
#endif
