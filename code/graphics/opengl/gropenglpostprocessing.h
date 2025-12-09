
#ifndef _GROPENGLPOSTPROCESSING_H
#define _GROPENGLPOSTPROCESSING_H

#include "globalincs/pstypes.h"
#include "graphics/opengl/gropenglshader.h"

void opengl_post_process_init();
void opengl_post_process_shutdown();

void gr_opengl_post_process_set_effect(const char *name, int x, const vec3d *rgb);
void gr_opengl_post_process_set_defaults();
void gr_opengl_post_process_save_zbuffer();
void gr_opengl_post_process_restore_zbuffer();
void gr_opengl_post_process_begin();
void gr_opengl_post_process_end();

void opengl_post_shader_header(SCP_stringstream &sflags, shader_type shader_t, int flags);

// Unit-test helper for exercising SMAA texture creation fallback paths
GLuint opengl_test_load_smaa_texture(GLsizei width, GLsizei height, GLenum format, const uint8_t* pixels = nullptr);

#endif	// _GROPENGLPOSTPROCESSING_H
