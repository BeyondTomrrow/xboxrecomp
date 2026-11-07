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

enum { HS_MAT_MATTE, HS_MAT_WOOD, HS_MAT_TILE, HS_MAT_MIRROR, HS_MAT_EMISSIVE, HS_MAT_FABRIC, HS_MAT_GLASS, HS_MAT_SCREEN };

typedef struct { float lo[3], hi[3]; uint32_t rgb; uint32_t mat; } XboxHostBox;
typedef struct { float pos[3], color[3], range; } XboxHostLight;

typedef struct {
    float eye[3], at[3], fov_y;              /* degrees, vertical */
    float sky[3], ground[3];                 /* hemisphere ambient */
    float time;                              /* seconds, for animated surfaces */
    int highlight;                           /* box index drawn with a soft glow, -1 none */
    int nbox, nlight;
    XboxHostBox box[XBOX_HOSTSCENE_BOXES];
    XboxHostLight light[XBOX_HOSTSCENE_LIGHTS];
} XboxHostScene;

#ifdef __cplusplus
extern "C" {
#endif
void xbox_HostSceneSet(const XboxHostScene *scene);
/* The trigger: a guest draw whose stage-0 texture is at one of these offsets (low 26 bits)
 * is replaced by the scene. The game side draws a small rect with a texture of its own. */
void xbox_HostSceneTrigger(const uint32_t *offsets, int n);
int  xbox_HostSceneDrawn(void);              /* frames drawn so far (tests and logs) */
#ifdef __cplusplus
}
#endif
#endif
