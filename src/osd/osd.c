#include <stdio.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <pthread.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/m64p_config.h"
#include "api/callbacks.h"

#include "osd_text.h"
#include "osd.h"

static const char *l_vertexShaderSource = "\
attribute vec4 vertex; // <vec2 pos, vec2 tex>\n\
varying vec2 TexCoords;\n\
\n\
uniform mat4 projection;\n\
uniform vec2 position;\n\
\n\
void main()\n\
{\n\
    gl_Position = projection * vec4(vertex.x+position.x, vertex.y+position.y, 0.0, 1.0);\n\
    TexCoords = vertex.zw;\n\
}  \0";

static const char *l_fragmentShaderSource = "\
varying vec2 TexCoords;\n\
\n\
uniform sampler2D text;\n\
uniform vec3 fgColor;\n\
uniform vec3 bgColor;\n\
uniform float alpha;\n\
\n\
void main()\n\
{\n\
    vec4 sampled = texture2D(text, TexCoords);\n\
    gl_FragColor = vec4(mix(bgColor, fgColor, sampled.r), alpha*sampled.a);\n\
}\0";

static unsigned int l_shaderProgram;
static int l_OsdInitialized = 0;
static int l_OsdFailedInit = 0;

static int l_width = 0;
static int l_height = 0;

static LIST_HEAD(l_messageQueue);
static LIST_HEAD(l_unusedMessages);

static void animation_fade(osd_message_t *);
static void osd_remove_message(osd_message_t *);
static osd_message_t * osd_message_valid(osd_message_t *);
static void draw_message(osd_message_t *msg, int width, int height);
static void osd_render_init();

static float fCornerScroll[OSD_NUM_CORNERS];

static osd_vertex *l_vertices;

static atlas *l_atlas;
static unsigned int l_vao, l_vbo;

static SDL_mutex *osd_list_lock;

void debug_dump()
{
    GLint height, width;
    GLint currentTexture, currentActiveTexture;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &currentActiveTexture);

    fprintf(stderr, "Binding texture %d\n", l_atlas->atlasTextureID);

    pglActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, l_atlas->atlasTextureID);

    fprintf(stderr, "Getting parameters\n");
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

    fprintf(stderr, "Texture size: %d, %d\n", width, height);

    if (width < 10000 && height < 10000)
    {

        unsigned char *buffer = malloc(height*width*2*sizeof(unsigned char));

        if (buffer == NULL) {
            fprintf(stderr, "Failed to allocate buffer\n");
        }

        fprintf(stderr, "Getting texture data\n");
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, buffer);

        fprintf(stderr, "Allocating line buffer\n");

        char *lineBuffer = malloc(100*4);
        
        if (lineBuffer == NULL) {
            fprintf(stderr, "Failed to allocate lineBuffer\n");
        }

        int rowSize = width;

        for (int y = 5; y < 6; y++)
        {
            char *bufPtr = lineBuffer;
            int rowStart=y*rowSize;
            for (int x = 200; x < 300; x++)
            {
                int a = sprintf(bufPtr, "%02X ",
                    buffer[rowStart+x]);
                bufPtr += a;
            }
            fprintf(stderr, "%s\n", lineBuffer);
        }

        free(lineBuffer);
        free(buffer);
    }

    pglActiveTexture(currentActiveTexture);
    glBindTexture(GL_TEXTURE_2D, currentTexture);
}

static void draw_message(osd_message_t *msg, int width, int height)
{
    int x, y;
    int borderSize = l_atlas->borderSize;
    int textHeight = l_atlas->textHeight;

    if (msg->textWidth == 0)
    {
        msg->textVertexCnt = OSD_VERTEX_BUF_SIZE/sizeof(osd_vertex);
        msg->textWidth = compile_text(msg->text, l_atlas, l_vertices, &msg->textVertexCnt);
        compile_border(msg->textWidth, l_atlas, &l_vertices[msg->textVertexCnt]);
        pglBufferSubData(GL_ARRAY_BUFFER, msg->vboOffset, (msg->textVertexCnt+24)*sizeof(osd_vertex), l_vertices);
    }

    switch(msg->corner)
    {
        case OSD_TOP_LEFT:
            x = borderSize;
            y = height - textHeight - 3*borderSize;
            msg->yOffset = -msg->yOffset;
            break;
        case OSD_TOP_CENTER:
            x = width/2 - msg->textWidth/2 - borderSize;
            y = height - textHeight - 3*borderSize;
            msg->yOffset = -msg->yOffset;
            break;
        case OSD_TOP_RIGHT:
            x = width - msg->textWidth - 3*borderSize;
            y = height - textHeight - 3*borderSize;
            msg->yOffset = -msg->yOffset;
            break;
        case OSD_MIDDLE_LEFT:
            x = l_atlas->borderSize;
            y = height/2 - textHeight/2 - borderSize;
            break;
        case OSD_MIDDLE_CENTER:
            x = width/2 - msg->textWidth/2 - borderSize;
            y = height/2 - textHeight/2 - borderSize;
            break;
        case OSD_MIDDLE_RIGHT:
            x = width - msg->textWidth - 3*borderSize;
            y = height/2 - textHeight/2 - borderSize;
            break;
        case OSD_BOTTOM_LEFT:
            x = l_atlas->borderSize;
            y = l_atlas->borderSize;
            break;
        case OSD_BOTTOM_CENTER:
            x = width/2 - msg->textWidth/2 - borderSize;
            y = l_atlas->borderSize;
            break;
        case OSD_BOTTOM_RIGHT:
            x = width - msg->textWidth - 3*borderSize;
            y = l_atlas->borderSize;
            break;
        default:
            DebugMessage(M64MSG_ERROR, "Invalid message corner %d", msg->corner);
            return;
    }

    y += msg->yOffset;

    pglUniform1f(pglGetUniformLocation(l_shaderProgram, "alpha"), msg->alpha);
    pglUniform2f(pglGetUniformLocation(l_shaderProgram, "position"), x, y);
    glBindTexture(GL_TEXTURE_2D, l_atlas->borderTextureID);
    glDrawArrays(GL_TRIANGLES, msg->vboOffset/sizeof(osd_vertex)+msg->textVertexCnt, 24);

    pglUniform2f(pglGetUniformLocation(l_shaderProgram, "position"), x+borderSize, y+borderSize);
    glBindTexture(GL_TEXTURE_2D, l_atlas->atlasTextureID);
    glDrawArrays(GL_TRIANGLES, msg->vboOffset/sizeof(osd_vertex), msg->textVertexCnt);
}

void osd_init(int width, int height)
{
    l_OsdInitialized = 0;
    l_OsdFailedInit = 0;
    l_width = width;
    l_height = height;
}

void osd_render_init()
{
    unsigned int vertexShader, fragmentShader;
    int success;
    char infoLog[512];
    const char *fontpath;

    if (l_OsdFailedInit)
        return;

    l_OsdFailedInit = 1;

    fprintf(stderr, "In osd_init thread: %ld\n", pthread_self());

    l_shaderProgram = 0;
    l_vertices = NULL;

    osd_gl_init();

    osd_list_lock = SDL_CreateMutex();
    if (!osd_list_lock)
    {
        DebugMessage(M64MSG_ERROR, "Could not create osd list lock");
        return;
    }

    fprintf(stderr, "Mutex created\n");

    for (int i = 0; i < OSD_NUM_MESSAGES; i++)
    {
        osd_message_t *msg = malloc(sizeof(osd_message_t));
        if (msg == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Could not allocate messages");
            return;
        }
        msg->vboOffset = i*OSD_VERTEX_BUF_SIZE;
        msg->text = NULL;
        list_add(&msg->list, &l_unusedMessages);
    }

    fprintf(stderr, "Messages created\n");

    l_vertices = malloc(OSD_VERTEX_BUF_SIZE);

    fprintf(stderr, "l_vertices allocated: %p\n", l_vertices);

    vertexShader = pglCreateShader(GL_VERTEX_SHADER);
    fprintf(stderr, "Vertex shader created\n");
    pglShaderSource(vertexShader, 1, &l_vertexShaderSource, NULL);
    fprintf(stderr, "Source added\n");
    pglCompileShader(vertexShader);
    fprintf(stderr, "Compiled\n");
    pglGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    fprintf(stderr, "Retrieved status\n");
    if (!success)
    {
        fprintf(stderr, "Not successful, getting log message\n");
        pglGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        fprintf(stderr, "Printing log message: %s\n", infoLog);
        DebugMessage(M64MSG_ERROR, "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n%s", infoLog);
        return;
    }

    fprintf(stderr, "Vertex shader done\n");

    fragmentShader = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(fragmentShader, 1, &l_fragmentShaderSource, NULL);
    pglCompileShader(fragmentShader);
    pglGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        pglGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        DebugMessage(M64MSG_ERROR, "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n%s", infoLog);
        pglDeleteShader(vertexShader);
        return;
    }

    fprintf(stderr, "Fragment shader created\n");

    l_shaderProgram = pglCreateProgram();
    pglAttachShader(l_shaderProgram, vertexShader);
    pglAttachShader(l_shaderProgram, fragmentShader);
    pglLinkProgram(l_shaderProgram);
    pglGetProgramiv(l_shaderProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        pglGetProgramInfoLog(l_shaderProgram, 512, NULL, infoLog);
        DebugMessage(M64MSG_ERROR, "ERROR::SHADER::PROGRAM::LINK_FAILED\n%s", infoLog);
        pglDeleteShader(vertexShader);
        pglDeleteShader(fragmentShader);
        return;
    }

    pglDeleteShader(vertexShader);
    pglDeleteShader(fragmentShader);

    fprintf(stderr, "Shader program created\n");

    vec3 fgColor = {OSD_FG_COLOR};
    vec3 bgColor = {OSD_BG_COLOR};
    GLint program;
    GLint vao, vbo;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);

    pglUseProgram(l_shaderProgram);
    pglUniform3f(pglGetUniformLocation(l_shaderProgram, "fgColor"), fgColor.x, fgColor.y, fgColor.z);
    pglUniform3f(pglGetUniformLocation(l_shaderProgram, "bgColor"), bgColor.x, bgColor.y, bgColor.z);

    fontpath = ConfigGetSharedDataFilepath(FONT_FILENAME);
    
    l_atlas = create_atlas(fontpath, (float)l_height*OSD_TEXT_HEIGHT_RATIO);
    if (l_atlas == NULL)
    {
        return;
    }

    fprintf(stderr, "Atlas created\n");

    debug_dump();

    pglGenVertexArrays(1, &l_vao);
    pglGenBuffers(1, &l_vbo);

    pglBindVertexArray(l_vao);

    pglBindBuffer(GL_ARRAY_BUFFER, l_vbo);

    pglBufferData(GL_ARRAY_BUFFER, OSD_VERTEX_BUF_SIZE*OSD_NUM_MESSAGES, NULL, GL_DYNAMIC_DRAW);

    pglVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(osd_vertex), (void*)0);
    pglEnableVertexAttribArray(0);

    pglUseProgram(program);
    pglBindBuffer(GL_ARRAY_BUFFER, vbo);
    pglBindVertexArray(vao);
    for (int i = 0; i < OSD_NUM_CORNERS; i++)
    {
        fCornerScroll[i] = 0.0f;
    }

    fprintf(stderr, "Initialized\n");

    l_OsdFailedInit = 0;
    l_OsdInitialized = 1;
}

void osd_exit(void)
{
    osd_message_t *msg, *safe;

    l_OsdInitialized = 0;

    pglDeleteVertexArrays(1, &l_vao);
    pglDeleteBuffers(1, &l_vbo);

    if (l_vertices != NULL)
    {
        free(l_vertices);
    }

    if (l_atlas != NULL)
    {
        if (l_atlas->atlasTextureID != 0)
        {
            glDeleteTextures(1, &l_atlas->atlasTextureID);
        }
        if (l_atlas->borderTextureID != 0)
        {
            glDeleteTextures(1, &l_atlas->borderTextureID);
        }
        if (l_atlas->chars != NULL)
        {
            free(l_atlas->chars);
        }
        free(l_atlas);
    }

    SDL_LockMutex(osd_list_lock);
    list_for_each_entry_safe_t(msg, safe, &l_messageQueue, osd_message_t, list)
    {
        osd_remove_message(msg);
    }
    list_for_each_entry_safe_t(msg, safe, &l_unusedMessages, osd_message_t, list)
    {
        if (!msg->user_managed)
        {
            free(msg);
        }
    }
    SDL_UnlockMutex(osd_list_lock);
}

void osd_render(void)
{
    static int width = 0, height = 0, x = 0, y = 0;
    static unsigned int lastRenderTime = 0;
    static unsigned int lastDebugTime = 0;
    unsigned int curRenderTime;

    curRenderTime = SDL_GetTicks();

    if (!l_OsdInitialized)
        osd_render_init();

    // if (l_OsdInitialized && curRenderTime - lastDebugTime >= 500)
    // {
    //     lastDebugTime = curRenderTime;
    //     debug_dump();
    // }

    if (!l_OsdInitialized || list_empty(&l_messageQueue))
    {
        return;
    }

    ivec4 viewport;
    GLint program;
    GLint vao, vbo;
    float fCornerPos[OSD_NUM_CORNERS];
    float msgHeight = l_atlas->textHeight + 3*l_atlas->borderSize;
    osd_message_t *msg, *safe;

    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    pglUseProgram(l_shaderProgram);

    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    pglBindVertexArray(l_vao);
    pglBindBuffer(GL_ARRAY_BUFFER, l_vbo);
    glGetIntegerv(GL_VIEWPORT, viewport.data);

    if (x != viewport.x || y != viewport.y || width != viewport.z
        || height != viewport.w)
    {
        x = viewport.x;
        y = viewport.y;
        width = viewport.z-x;
        height = viewport.w-y;
        float projection[] = { 2.0f/(float)width, 0, 0, 0,
                               0, 2.0f/(float)height, 0, 0,
                               0, 0, -1, 0,
                               -(float)(x+viewport.z)/(float)width, -(float)(y+viewport.w)/(float)height, 0, 1 };
        pglUniformMatrix4fv(pglGetUniformLocation(l_shaderProgram, "projection"), 1, GL_FALSE, projection);
    }

    GLint currentTexture, currentActiveTexture;
    GLint srcRgb, dstRgb, srcAlpha, dstAlpha;
    GLboolean blend;

    glGetBooleanv(GL_BLEND, &blend);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &currentActiveTexture);

    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    for (int i = 0; i < OSD_NUM_CORNERS; i++)
    {
        fCornerPos[i] = 0.0f;
    }

    SDL_LockMutex(osd_list_lock);

    list_for_each_entry_safe_t(msg, safe, &l_messageQueue, osd_message_t, list)
    {
        msg->elapsedTime += curRenderTime - lastRenderTime;
        if (msg->timeout[msg->state] != OSD_INFINITE_TIMEOUT &&
            msg->elapsedTime >= msg->timeout[msg->state])
        {
            msg->state++;
            if (msg->state >= OSD_NUM_STATES)
            {
                osd_remove_message(msg);
                continue;
            }
            msg->elapsedTime = 0;
        }

        msg->yOffset = fCornerPos[msg->corner];
        //Only scroll corner messages, not middle messages
        if (msg->corner < OSD_MIDDLE_LEFT || msg->corner > OSD_MIDDLE_RIGHT)
        {
            msg->yOffset += fCornerScroll[msg->corner] * msgHeight;
        }

        animation_fade(msg);

        draw_message(msg, viewport.z, viewport.w);

        fCornerPos[msg->corner] += msgHeight;
    }

    pglBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);

    if (blend == GL_FALSE)
        glDisable(GL_BLEND);

    pglActiveTexture(currentActiveTexture);
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    pglBindBuffer(GL_ARRAY_BUFFER, vbo);
    pglBindVertexArray(vao);

    float scrollAdd = (float)(curRenderTime-lastRenderTime)/OSD_SCROLL_MSEC;

    for (int i = 0; i < OSD_NUM_CORNERS; i++)
    {
        fCornerScroll[i] += scrollAdd;
        if (fCornerScroll[i] >= 0)
        {
            fCornerScroll[i] = 0;
        }
    }

    SDL_UnlockMutex(osd_list_lock);

    //Cleanup back to original state
    pglUseProgram(program);

    lastRenderTime = curRenderTime;
}

// creates a new osd_message_t, adds it to the message queue and returns it in case
// the user wants to modify its parameters. Note, if the message can't be created,
// NULL is returned.
osd_message_t * osd_new_message(enum osd_corner eCorner, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    osd_message_t *msg = NULL;

    if (!l_OsdInitialized) return msg;

    // fprintf(stderr, "Dumping Texture\n");
    // dump_texture(l_atlas);
    // fprintf(stderr, "Done\n");

    SDL_LockMutex(osd_list_lock);

    if (!list_empty(&l_unusedMessages))
    {
        msg = list_first_entry(&l_unusedMessages, osd_message_t, list);
        list_del_init(&msg->list);

        va_start(ap, fmt);
        vsnprintf(buf, 1024, fmt, ap);
        buf[1023] = 0;
        va_end(ap);

        // set default values
        msg->text = strdup(buf);

        msg->user_managed = 0;
        msg->elapsedTime = 0;
        msg->yOffset = 0.0f;
        msg->alpha = 1.0f;
        msg->corner = eCorner;
        msg->state = OSD_APPEAR;
        fCornerScroll[eCorner] -= 1.0;  // start this one before the beginning of the list and scroll it in

        if (eCorner >= OSD_MIDDLE_LEFT && eCorner <= OSD_MIDDLE_RIGHT)
        {
            msg->timeout[OSD_APPEAR] = 333;
            msg->timeout[OSD_DISPLAY] = 1000;
            msg->timeout[OSD_DISAPPEAR] = 333;
        }
        else
        {
            msg->timeout[OSD_APPEAR] = 333;
            msg->timeout[OSD_DISPLAY] = 1500;
            msg->timeout[OSD_DISAPPEAR] = 666;
        }

        msg->textWidth = 0;
        // add to message queue
        list_add(&msg->list, &l_messageQueue);
    }
        
    SDL_UnlockMutex(osd_list_lock);

    return msg;
}

// update message string
void osd_update_message(osd_message_t *msg, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    if (!l_OsdInitialized || !msg) return;

    va_start(ap, fmt);
    vsnprintf(buf, 1024, fmt, ap);
    buf[1023] = 0;
    va_end(ap);

    free(msg->text);
    msg->text = strdup(buf);

    // reset display time counter
    if (msg->state >= OSD_DISPLAY)
    {
        msg->state = OSD_DISPLAY;
        msg->elapsedTime = 0;
    }

    msg->textWidth = 0;
    SDL_LockMutex(osd_list_lock);
    if (!osd_message_valid(msg))
    {
        list_del_init(&msg->list);
        list_add(&msg->list, &l_messageQueue);
    }
    SDL_UnlockMutex(osd_list_lock);
}

// return message pointer if valid (in the OSD list), otherwise return NULL
static osd_message_t * osd_message_valid(osd_message_t *testmsg)
{
    osd_message_t *msg;

    if (!l_OsdInitialized || !testmsg) return NULL;

    list_for_each_entry_t(msg, &l_messageQueue, osd_message_t, list) {
        if (msg == testmsg)
            return testmsg;
    }

    return NULL;
}

// remove message from message queue
static void osd_remove_message(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    if (msg->text != NULL)
    {
        free(msg->text);
    }
    
    msg->text = NULL;
    list_del_init(&msg->list);
    if (!msg->user_managed)
    {
        list_add(&msg->list, &l_unusedMessages);
    }
}

// remove message from message queue and free it
void osd_delete_message(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;
    msg->user_managed = 0;
    SDL_LockMutex(osd_list_lock);
    osd_remove_message(msg);
    SDL_UnlockMutex(osd_list_lock);
}

// set message so it doesn't automatically expire in a certain number of frames.
void osd_message_set_static(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    msg->timeout[OSD_DISPLAY] = OSD_INFINITE_TIMEOUT;
    msg->state = OSD_DISPLAY;
    msg->elapsedTime = 0;
}

// set message so it doesn't automatically get freed when finished transition.
void osd_message_set_user_managed(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    msg->user_managed = 1;
}

// fade in/out animation handler
static void animation_fade(osd_message_t *msg)
{
    float elapsedTime;
    float timeout = (float)msg->timeout[msg->state];

    if (msg->timeout[msg->state] != 0)
    {
        switch(msg->state)
        {
            case OSD_DISAPPEAR:
                elapsedTime = (float)(timeout - msg->elapsedTime);
                break;
            case OSD_APPEAR:
                elapsedTime = (float)msg->elapsedTime;
                break;
            default:
                elapsedTime = timeout; //Bit hacky but force alpha to 1
                break;
        }

        msg->alpha = elapsedTime / timeout;
    }
    else
    {
        msg->alpha = 1.0f;
    }
}