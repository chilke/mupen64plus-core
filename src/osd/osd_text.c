#include <stdio.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "api/callbacks.h"

#include "osd_text.h"
#include "osd_gl.h"

static unsigned char denormalize_uchar(float f, unsigned char min, unsigned char max);
static float integrate_arc(float r, float x0, float x1);
static float **perc_inside_arc(int size, int radius);
static unsigned char *create_corner_bitmap(int size, int borderWidth);

character *get_atlas_character(atlas *a, char c)
{
    if (c < ATLAS_START_CHAR || c >= ATLAS_START_CHAR+ATLAS_NUM_CHARS)
    {
        return NULL;
    }

    return &(a->chars[(int)c-ATLAS_START_CHAR]);
}

atlas *create_atlas(const char *fontPath, int pixelSizeY)
{
    FT_Library ft;
    FT_Face face;

    atlas *a;

    int rowW, rowH, maxW, totalH, imgW, imgH, w, rows, curY;
    int maxBearingY, maxDropY, bearingY, bearingX, dropY;

    if (FT_Init_FreeType(&ft))
    {
        DebugMessage(M64MSG_ERROR, "ERROR::FREETYPE: Could not init FreeType Library");
        return NULL;
    }

    if (FT_New_Face(ft, fontPath, 0, &face))
    {
        DebugMessage(M64MSG_ERROR, "ERROR::FREETYPE: Failed to load font");
        return NULL;
    }

    FT_Set_Pixel_Sizes(face, 0, pixelSizeY);

    a = (atlas *)malloc(sizeof(atlas));
    a->chars = malloc(ATLAS_NUM_CHARS * sizeof(character));

    //First calculate the required size for the texture
    rowH = maxW = 0;
    rows = rowW = totalH = 1;
    maxBearingY = maxDropY = 0;
    for (int c = ATLAS_START_CHAR; c < ATLAS_START_CHAR + ATLAS_NUM_CHARS; c++)
    {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
        {
            DebugMessage(M64MSG_ERROR, "ERROR::FREETYPE: Faled to load Glyph %d", c);
            continue;
        }

        w = face->glyph->advance.x>>6;
        imgH = face->glyph->bitmap.rows;
        bearingY = face->glyph->bitmap_top;
        dropY = imgH - bearingY;

        if (dropY > maxDropY)
        {
            maxDropY = dropY;
        }
        if (bearingY > maxBearingY)
        {
            maxBearingY = bearingY;
        }

        if (rowW + w + 1 > MAX_ATLAS_WIDTH)
        {
            if (rowW > maxW)
            {
                maxW = rowW;
            }
            rowW = 1;
            rows++;
        }

        rowW += w + 1;
    }

    if (rowW > maxW)
    {
        maxW = rowW;
    }

    rowH = maxBearingY + maxDropY;
    totalH = rows * (rowH+1) + 1;

    a->textHeight = rowH;
    a->textNormHeight = (float)rowH/(float)totalH;
    a->maxBearingY = maxBearingY;
    a->maxDropY = maxDropY;
    a->textureWidth = maxW;
    a->textureHeight = totalH;

    unsigned char *buffer = malloc(maxW*totalH*sizeof(unsigned char));
    memset(buffer, 0, maxW*totalH*sizeof(unsigned char));

    a->atlasTextureID = 0;
    glGenTextures(1, &a->atlasTextureID);
    fprintf(stderr, "Generated texture %d\n",
        a->atlasTextureID);
    a->atlasTextureID = 0;
    glGenTextures(1, &a->atlasTextureID);
    fprintf(stderr, "Generated texture %d\n",
        a->atlasTextureID);
    a->atlasTextureID = 0;
    glGenTextures(1, &a->atlasTextureID);
    fprintf(stderr, "Generated texture %d\n",
        a->atlasTextureID);
    
    GLint currentTexture, currentActiveTexture;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &currentActiveTexture);

    glBindTexture(GL_TEXTURE_2D, a->atlasTextureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, maxW, totalH, 0, GL_RED, GL_UNSIGNED_BYTE, buffer);

    free(buffer);

    rowW = curY = 1;

    for (int c = ATLAS_START_CHAR; c < ATLAS_START_CHAR + ATLAS_NUM_CHARS; c++)
    {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
        {
            continue;
        }

        imgW = face->glyph->bitmap.width;
        w = face->glyph->advance.x>>6;
        imgH = face->glyph->bitmap.rows;
        bearingY = face->glyph->bitmap_top;
        bearingX = face->glyph->bitmap_left;
        dropY = imgH - bearingY;

        if (rowW + w + 1 > MAX_ATLAS_WIDTH)
        {
            rowW = 1;
            curY += rowH+1;
        }

        a->chars[c-ATLAS_START_CHAR].width = w;
        a->chars[c-ATLAS_START_CHAR].normWidth = (float)w/(float)maxW;
        a->chars[c-ATLAS_START_CHAR].normPos.x = (float)rowW/(float)maxW;
        a->chars[c-ATLAS_START_CHAR].normPos.y = (float)curY/(float)totalH;

        glTexSubImage2D(GL_TEXTURE_2D, 0, rowW+bearingX, curY+maxBearingY-bearingY, imgW, imgH, GL_RED, GL_UNSIGNED_BYTE, face->glyph->bitmap.buffer);

        rowW += w + 1;
    }

    int borderSize = (int)(ceil((float)a->textHeight*BORDER_RATIO));
    int borderWidth = (int)(ceil((float)borderSize*OUTLINE_RATIO));

    a->borderSize = borderSize;

    int width, height;
    buffer = create_corner_bitmap(borderSize, borderWidth);
    height = borderSize;
    width = borderSize+1;

    a->borderTextureID = 0;
    glGenTextures(1, &a->borderTextureID);
    fprintf(stderr, "Generated texture %d\n",
        a->borderTextureID);
    glBindTexture(GL_TEXTURE_2D, a->borderTextureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

    free(buffer);

    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    pglActiveTexture(currentActiveTexture);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    return a;
}

//Compile the vertices for a given line of text and return the width
int compile_text(char *text, atlas *a, osd_vertex *vertices, int *verticesLength)
{
    char *c = text;
    float normH = a->textNormHeight;
    int h = a->textHeight;
    int i = 0;
    int x = 0;
    while (*c != '\0')
    {
        character *ch = get_atlas_character(a, *c);
        if (ch != NULL)
        {
            if (i + 6 >= *verticesLength)
            {
                break;
            }

            float normX = ch->normPos.x;
            float normY = ch->normPos.y;
            float normW = ch->normWidth;
            int w = ch->width;

            vertices[i].x = x;
            vertices[i].y = h;
            vertices[i].texX = normX;
            vertices[i].texY = normY;
            i++;

            vertices[i].x = x;
            vertices[i].y = 0;
            vertices[i].texX = normX;
            vertices[i].texY = normY+normH;
            i++;

            vertices[i].x = x+w;
            vertices[i].y = 0;
            vertices[i].texX = normX+normW;
            vertices[i].texY = normY+normH;
            i++;

            vertices[i].x = x;
            vertices[i].y = h;
            vertices[i].texX = normX;
            vertices[i].texY = normY;
            i++;

            vertices[i].x = x+w;
            vertices[i].y = 0;
            vertices[i].texX = normX+normW;
            vertices[i].texY = normY+normH;
            i++;

            vertices[i].x = x+w;
            vertices[i].y = h;
            vertices[i].texX = normX+normW;
            vertices[i].texY = normY;
            i++;

            x += w;
        }
        c++;
    }

    *verticesLength = i;

    return x;
}

void compile_border(int width, atlas *a, osd_vertex *vertices)
{
    int i = 0;
    int borderSize = a->borderSize;
    width += borderSize;
    float normWidth = (float)width/(float)(borderSize+1);

    int height = a->textHeight+borderSize;

    float normHeight = (float)height/(float)(borderSize+1);

    vertices[i].x = 0;
    vertices[i].y = height+borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = 0;
    vertices[i].y = height;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = height;
    vertices[i].texX = normWidth;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = 0;
    vertices[i].y = height+borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = height;
    vertices[i].texX = normWidth;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = height+borderSize;
    vertices[i].texX = normWidth;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = height+borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = borderSize;
    vertices[i].texX = normHeight;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = borderSize;
    vertices[i].texX = normHeight;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = height+borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = borderSize;
    vertices[i].texX = normHeight;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width;
    vertices[i].y = height+borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = 0;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = borderSize;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = borderSize;
    vertices[i].texX = normWidth;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = width+borderSize;
    vertices[i].y = 0;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = borderSize;
    vertices[i].texX = normWidth;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = 0;
    vertices[i].texX = normWidth;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = 0;
    vertices[i].y = 0;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = 0;
    vertices[i].y = height;
    vertices[i].texX = normHeight;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = height;
    vertices[i].texX = normHeight;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = 0;
    vertices[i].y = 0;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 0.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = height;
    vertices[i].texX = normHeight;
    vertices[i].texY = 1.0f;
    i++;

    vertices[i].x = borderSize;
    vertices[i].y = 0;
    vertices[i].texX = 0.0f;
    vertices[i].texY = 1.0f;
    i++;

}

static unsigned char denormalize_uchar(float f, unsigned char min, unsigned char max)
{
    int i = (int)(f * (float)(max-min)) + min;
    if (i < min) {
        i = min;
    }
    else if (i > max)
    {
        i = max;
    }

    return (unsigned char)i;
}

//Assumes quarter upper left quarter circle starting at 0,0
//And rising up and to the right from there
static float integrate_arc(float r, float x0, float x1)
{
    float x = r-x1;
    float i0 = r*r*asin(x/r)/2 + x*sqrt(r*r-x*x)/2;
    x = r-x0;
    float i1 = r*r*asin(x/r)/2 + x*sqrt(r*r-x*x)/2;

    return i1-i0;
}

static float **perc_inside_arc(int size, int radius)
{
    // Where the arc crosses each horizontal y gridline
    float *xIntersects = malloc((radius+1)*sizeof(float));
    float **percInside = malloc(size*size*sizeof(float));

    float r = (float)radius;
    float rSq = r*r;

    for (int i = 0; i <= radius; i++)
    {
        float f = (float)i;
        xIntersects[i] = r - sqrt(rSq - f*f);
    }

    for (int y = 0; y < size; y++)
    {
        percInside[y] = malloc(size*sizeof(float));
        for (int ix = 0; ix < size; ix++)
        {
            int x = ix+radius-size;
            if (x < 0 || y >= radius)
            {
                percInside[y][ix] = 0.0f;
                continue;
            }
            float startX = (float)x;
            float endX = (float)x+1;

            if (xIntersects[y] > endX)
            {
                percInside[y][ix] = 0.0f;
            }
            else if (xIntersects[y+1] < startX)
            {
                percInside[y][ix] = 1.0f;
            }
            else
            {
                float p = 0.0f;
                if (xIntersects[y] > startX)
                {
                    startX = xIntersects[y];
                }

                if (xIntersects[y+1] < endX)
                {
                    p = endX - xIntersects[y+1];
                    endX = xIntersects[y+1];
                }

                //Now calculate area under curve from startX to endX
                // and then subtract endX-startX*y
                p += integrate_arc(r, startX, endX);
                p -= (endX-startX)*(float)y;

                percInside[y][ix] = p;
            }
        }
    }

    free(xIntersects);

    return percInside;
}

static unsigned char *create_corner_bitmap(int size, int borderWidth)
{
    //Origin is left edge with arc rising up and right from there
    //Drawing is of upper left hand quarter of a circle

    unsigned char *buffer = malloc(size*(size+1)*4 * sizeof(unsigned char));
    float **insideOuter = perc_inside_arc(size, size);
    float **insideInner = perc_inside_arc(size, size-borderWidth);

    int i = 0;

    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            //Red comes from percent outside inner radius
            //or 1 - percent inside inner radius
            buffer[i++] = denormalize_uchar(1-insideInner[size-y-1][x], 0, 255);
            buffer[i++] = 0;
            buffer[i++] = 0;
            buffer[i++] = denormalize_uchar(insideOuter[size-y-1][x], 0, 255);
        }

        if (y < borderWidth)
        {
            buffer[i++] = 255;
        }
        else
        {
            buffer[i++] = 0;
        }
        buffer[i++] = 0;
        buffer[i++] = 0;
        buffer[i++] = 255;
    }

    for (int i = 0; i < size; i++)
    {
        free(insideOuter[i]);
    }
    free(insideOuter);

    return buffer;
}