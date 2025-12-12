#include <gtest/gtest.h>

#include "graphics/opengl/gropenglpostprocessing.h"
#include "graphics/opengl/gropenglstate.h"
#include "graphics/opengl/SmaaAreaTex.h"
#include "graphics/opengl/SmaaSearchTex.h"

// We stub the GLAD function pointers to avoid needing a real GL context.

namespace {

// Recorded values for the texture allocation we care about
static GLsizei recorded_width  = -1;
static GLsizei recorded_height = -1;
static GLint recorded_internal = -1;
static GLenum recorded_format  = 0;

// --- GL stubs ---
void APIENTRY stub_glActiveTexture(GLenum) {}
void APIENTRY stub_glBindTexture(GLenum, GLuint) {}
void APIENTRY stub_glTexParameteri(GLenum, GLenum, GLint) {}
void APIENTRY stub_glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) {}
void APIENTRY stub_glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei) {}
void APIENTRY stub_glDisable(GLenum) {}
void APIENTRY stub_glEnable(GLenum) {}
void APIENTRY stub_glBlendFunc(GLenum, GLenum) {}
void APIENTRY stub_glColorMask(GLboolean, GLboolean, GLboolean, GLboolean) {}
void APIENTRY stub_glDepthMask(GLboolean) {}
void APIENTRY stub_glDepthFunc(GLenum) {}
void APIENTRY stub_glFrontFace(GLenum) {}
void APIENTRY stub_glCullFace(GLenum) {}
void APIENTRY stub_glBlendEquationSeparate(GLenum, GLenum) {}
void APIENTRY stub_glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum) {}
void APIENTRY stub_glLineWidth(GLfloat) {}
void APIENTRY stub_glStencilMask(GLuint) {}
void APIENTRY stub_glStencilFunc(GLenum, GLint, GLuint) {}
void APIENTRY stub_glStencilOp(GLenum, GLenum, GLenum) {}
void APIENTRY stub_glPolygonMode(GLenum, GLenum) {}
void APIENTRY stub_glBlendColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
void APIENTRY stub_glGetFloatv(GLenum, GLfloat* value) { if (value) *value = 1.0f; }
void APIENTRY stub_glUseProgram(GLuint) {}
void APIENTRY stub_glBindFramebuffer(GLenum, GLuint) {}
void APIENTRY stub_glGenTextures(GLsizei n, GLuint* textures) {
	if (textures) {
		for (GLsizei i = 0; i < n; ++i) {
			textures[i] = 100 + i; // arbitrary non-zero id
		}
	}
}

void APIENTRY stub_glTexImage2D(GLenum /*target*/, GLint /*level*/, GLint internalformat, GLsizei width, GLsizei height,
                                GLint /*border*/, GLenum format, GLenum /*type*/, const void* /*pixels*/) {
	recorded_width   = width;
	recorded_height  = height;
	recorded_internal = internalformat;
	recorded_format  = format;
}

} // namespace

class SmaaFallbackAllocTest : public ::testing::Test {
 protected:
	void SetUp() override {
		// Force the immutable-storage path off to exercise the fallback branch
		GLAD_GL_ARB_texture_storage = 0;

		// Wire GLAD pointers to our stubs
		glad_glActiveTexture         = stub_glActiveTexture;
		glad_glBindTexture           = stub_glBindTexture;
		glad_glTexParameteri         = stub_glTexParameteri;
		glad_glTexSubImage2D         = stub_glTexSubImage2D;
		glad_glTexStorage2D          = stub_glTexStorage2D;
		glad_glTexImage2D            = stub_glTexImage2D;
		glad_glDisable               = stub_glDisable;
		glad_glEnable                = stub_glEnable;
		glad_glBlendFunc             = stub_glBlendFunc;
		glad_glColorMask             = stub_glColorMask;
		glad_glDepthMask             = stub_glDepthMask;
		glad_glDepthFunc             = stub_glDepthFunc;
		glad_glFrontFace             = stub_glFrontFace;
		glad_glCullFace              = stub_glCullFace;
		glad_glBlendEquationSeparate = stub_glBlendEquationSeparate;
		glad_glBlendFuncSeparate     = stub_glBlendFuncSeparate;
		glad_glLineWidth             = stub_glLineWidth;
		glad_glStencilMask           = stub_glStencilMask;
		glad_glStencilFunc           = stub_glStencilFunc;
		glad_glStencilOp             = stub_glStencilOp;
		glad_glPolygonMode           = stub_glPolygonMode;
		glad_glBlendColor            = stub_glBlendColor;
		glad_glGetFloatv             = stub_glGetFloatv;
		glad_glUseProgram            = stub_glUseProgram;
		glad_glBindFramebuffer       = stub_glBindFramebuffer;
		glad_glGenTextures           = stub_glGenTextures;

		// GL state needs at least one texture unit to be initialized
		GL_state.Texture.init(1);

		// reset recorded values
		recorded_width = recorded_height = -1;
		recorded_internal = -1;
		recorded_format = 0;
	}
};

TEST_F(SmaaFallbackAllocTest, UsesProvidedDimensionsWhenTextureStorageUnavailable)
{
	const GLsizei expected_w = AREATEX_WIDTH;
	const GLsizei expected_h = AREATEX_HEIGHT;

	// Exercise the same helper used by production SMAA setup
	opengl_test_load_smaa_texture(expected_w, expected_h, GL_RG8, areaTexBytes);

	EXPECT_EQ(recorded_width, expected_w);
	EXPECT_EQ(recorded_height, expected_h);
	EXPECT_EQ(recorded_internal, GL_RG8);
	EXPECT_EQ(recorded_format, GL_RG);
}
