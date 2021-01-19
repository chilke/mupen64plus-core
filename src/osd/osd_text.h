#ifndef __OSD_TEXT_H__
#define __OSD_TEXT_H__

#include "SDL2/SDL_opengl.h"

#define BORDER_RATIO 0.2f
#define OUTLINE_RATIO 0.2f

#define MAX_ATLAS_WIDTH 1024

#define ATLAS_START_CHAR 32
#define ATLAS_NUM_CHARS 95

typedef union vec2 {
    float data[2];
    struct { float x, y; };
    struct { float w, h; };
} vec2;

typedef union vec3 {
    float data[3];
    struct { float x, y, z; };
} vec3;

typedef union ivec4 {
    GLint data[4];
    struct { int x, y, z, w; };
} ivec4;

typedef struct osd_vertex {
    GLfloat x, y;
    GLfloat texX, texY;
} osd_vertex;

typedef struct character {
    int width;
    float normWidth;
    vec2 normPos;
} character;

typedef struct atlas {
    character *chars;
    unsigned int atlasTextureID;
    unsigned int borderTextureID;
    int borderSize;
    int textHeight;
    int maxBearingY;
    int maxDropY;
    int textureWidth;
    int textureHeight;
    float textNormHeight;
} atlas;

character *get_atlas_character(atlas *a, char c);
atlas *create_atlas(const char *fontPath, int pixelSizeY);
int compile_text(char *text, atlas *a, osd_vertex *vertices, int *verticesLength);
void compile_border(int width, atlas *a, osd_vertex *vertices);

#endif