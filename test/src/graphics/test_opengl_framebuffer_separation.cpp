/**
 * FILENAME: test/src/graphics/test_opengl_framebuffer_separation.cpp
 *
 * PURPOSE: Validate the framebuffer read/draw separation using
 *          std::pair<GLuint, GLuint> for tracking separate read and draw buffers.
 *          This architectural change allows independent binding of read and draw
 *          framebuffers as required by operations like glBlitFramebuffer.
 *
 * DEPENDENCIES:
 * - gtest
 * - std::pair
 *
 * KEY TESTS:
 * 1. FramebufferState_SeparateReadDraw_IndependentValues: Validates separation
 * 2. FramebufferState_BindReadBuffer_DoesNotAffectDraw: Validates independence
 * 3. FramebufferState_BindBoth_UpdatesBoth: Validates combined binding
 *
 * NOTES:
 * - These tests validate the state tracking logic without requiring OpenGL context.
 * - Actual OpenGL validation requires integration tests with a valid GL context.
 */

#include <gtest/gtest.h>
#include <utility>
#include <stack>
#include <cstdint>

namespace {

// Type alias matching the codebase change
using FramebufferBinding = std::pair<uint32_t, uint32_t>;  // {read, draw}

// Constants (simulating GL constants)
constexpr uint32_t GL_NONE = 0;
constexpr uint32_t DEFAULT_FRAMEBUFFER = 0;

// Framebuffer state tracker
class FramebufferStateTracker {
public:
    FramebufferStateTracker()
        : m_current{DEFAULT_FRAMEBUFFER, DEFAULT_FRAMEBUFFER}
    {}

    // Get current read framebuffer
    uint32_t getReadBuffer() const { return m_current.first; }

    // Get current draw framebuffer
    uint32_t getDrawBuffer() const { return m_current.second; }

    // Get both as pair
    FramebufferBinding getCurrentBinding() const { return m_current; }

    // Bind read framebuffer only (GL_READ_FRAMEBUFFER)
    void bindReadBuffer(uint32_t fbo) {
        m_current.first = fbo;
    }

    // Bind draw framebuffer only (GL_DRAW_FRAMEBUFFER)
    void bindDrawBuffer(uint32_t fbo) {
        m_current.second = fbo;
    }

    // Bind both read and draw (GL_FRAMEBUFFER)
    void bindBoth(uint32_t fbo) {
        m_current.first = fbo;
        m_current.second = fbo;
    }

    // Set from pair
    void setBinding(const FramebufferBinding& binding) {
        m_current = binding;
    }

    // Check if read and draw are same
    bool isUnified() const {
        return m_current.first == m_current.second;
    }

    // Push current state for later restoration
    void push() {
        m_stack.push(m_current);
    }

    // Pop and restore previous state
    bool pop() {
        if (m_stack.empty()) {
            return false;
        }
        m_current = m_stack.top();
        m_stack.pop();
        return true;
    }

private:
    FramebufferBinding m_current;
    std::stack<FramebufferBinding> m_stack;
};

// Simulated framebuffer handles
constexpr uint32_t FBO_SCENE = 1;
constexpr uint32_t FBO_POST_PROCESS = 2;
constexpr uint32_t FBO_SHADOW_MAP = 3;
constexpr uint32_t FBO_BLUR_A = 4;
constexpr uint32_t FBO_BLUR_B = 5;

} // namespace

class OpenGLFramebufferSeparationTest : public ::testing::Test {
protected:
    FramebufferStateTracker tracker;

    void SetUp() override {}
    void TearDown() override {}
};

// Test: Initial state is default framebuffer for both
TEST_F(OpenGLFramebufferSeparationTest, InitialState_BothAreDefault)
{
    EXPECT_EQ(tracker.getReadBuffer(), DEFAULT_FRAMEBUFFER);
    EXPECT_EQ(tracker.getDrawBuffer(), DEFAULT_FRAMEBUFFER);
    EXPECT_TRUE(tracker.isUnified());
}

// Test: Bind read buffer independently
TEST_F(OpenGLFramebufferSeparationTest, BindReadBuffer_DoesNotAffectDraw)
{
    tracker.bindDrawBuffer(FBO_SCENE);
    tracker.bindReadBuffer(FBO_POST_PROCESS);

    EXPECT_EQ(tracker.getReadBuffer(), FBO_POST_PROCESS)
        << "Read buffer should be FBO_POST_PROCESS";
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_SCENE)
        << "Draw buffer should remain FBO_SCENE";
    EXPECT_FALSE(tracker.isUnified());
}

// Test: Bind draw buffer independently
TEST_F(OpenGLFramebufferSeparationTest, BindDrawBuffer_DoesNotAffectRead)
{
    tracker.bindReadBuffer(FBO_SHADOW_MAP);
    tracker.bindDrawBuffer(FBO_BLUR_A);

    EXPECT_EQ(tracker.getReadBuffer(), FBO_SHADOW_MAP)
        << "Read buffer should remain FBO_SHADOW_MAP";
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_BLUR_A)
        << "Draw buffer should be FBO_BLUR_A";
    EXPECT_FALSE(tracker.isUnified());
}

// Test: Bind both simultaneously
TEST_F(OpenGLFramebufferSeparationTest, BindBoth_UpdatesBothBuffers)
{
    tracker.bindReadBuffer(FBO_BLUR_A);
    tracker.bindDrawBuffer(FBO_BLUR_B);

    // Now bind both to same
    tracker.bindBoth(FBO_SCENE);

    EXPECT_EQ(tracker.getReadBuffer(), FBO_SCENE);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_SCENE);
    EXPECT_TRUE(tracker.isUnified());
}

// Test: Pair storage and retrieval
TEST_F(OpenGLFramebufferSeparationTest, GetCurrentBinding_ReturnsPair)
{
    tracker.bindReadBuffer(FBO_BLUR_A);
    tracker.bindDrawBuffer(FBO_BLUR_B);

    FramebufferBinding binding = tracker.getCurrentBinding();

    EXPECT_EQ(binding.first, FBO_BLUR_A);
    EXPECT_EQ(binding.second, FBO_BLUR_B);
}

// Test: Set binding from pair
TEST_F(OpenGLFramebufferSeparationTest, SetBinding_AppliesPairValues)
{
    FramebufferBinding newBinding{FBO_SHADOW_MAP, FBO_POST_PROCESS};
    tracker.setBinding(newBinding);

    EXPECT_EQ(tracker.getReadBuffer(), FBO_SHADOW_MAP);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_POST_PROCESS);
}

// Test: Push/pop state preservation
TEST_F(OpenGLFramebufferSeparationTest, PushPop_PreservesState)
{
    tracker.bindReadBuffer(FBO_SCENE);
    tracker.bindDrawBuffer(FBO_POST_PROCESS);

    // Push current state
    tracker.push();

    // Change to different state
    tracker.bindBoth(FBO_SHADOW_MAP);
    EXPECT_EQ(tracker.getReadBuffer(), FBO_SHADOW_MAP);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_SHADOW_MAP);

    // Pop to restore
    EXPECT_TRUE(tracker.pop());
    EXPECT_EQ(tracker.getReadBuffer(), FBO_SCENE);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_POST_PROCESS);
}

// Test: Pop on empty stack
TEST_F(OpenGLFramebufferSeparationTest, PopEmptyStack_ReturnsFalse)
{
    EXPECT_FALSE(tracker.pop());
}

// Test: Multiple push/pop levels
TEST_F(OpenGLFramebufferSeparationTest, PushPop_MultipleLevel_LIFO)
{
    tracker.bindBoth(FBO_SCENE);
    tracker.push();

    tracker.bindBoth(FBO_POST_PROCESS);
    tracker.push();

    tracker.bindBoth(FBO_SHADOW_MAP);
    EXPECT_EQ(tracker.getReadBuffer(), FBO_SHADOW_MAP);

    EXPECT_TRUE(tracker.pop());
    EXPECT_EQ(tracker.getReadBuffer(), FBO_POST_PROCESS);

    EXPECT_TRUE(tracker.pop());
    EXPECT_EQ(tracker.getReadBuffer(), FBO_SCENE);

    EXPECT_FALSE(tracker.pop());  // Stack now empty
}

// Test: Blit scenario - read from one, draw to another
TEST_F(OpenGLFramebufferSeparationTest, BlitScenario_ReadFromDrawTo)
{
    // Typical glBlitFramebuffer setup:
    // - Read from the scene framebuffer
    // - Draw to the post-process framebuffer
    tracker.bindReadBuffer(FBO_SCENE);
    tracker.bindDrawBuffer(FBO_POST_PROCESS);

    FramebufferBinding blit_setup = tracker.getCurrentBinding();

    EXPECT_EQ(blit_setup.first, FBO_SCENE)
        << "Should read from scene FBO";
    EXPECT_EQ(blit_setup.second, FBO_POST_PROCESS)
        << "Should draw to post-process FBO";
    EXPECT_FALSE(tracker.isUnified())
        << "Blit requires separate read/draw FBOs";
}

// Test: Ping-pong blur scenario
TEST_F(OpenGLFramebufferSeparationTest, PingPongScenario_AlternateBuffers)
{
    // Common pattern in blur effects:
    // Pass 1: Read A, Draw B
    // Pass 2: Read B, Draw A

    // Pass 1
    tracker.bindReadBuffer(FBO_BLUR_A);
    tracker.bindDrawBuffer(FBO_BLUR_B);
    EXPECT_EQ(tracker.getReadBuffer(), FBO_BLUR_A);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_BLUR_B);

    // Pass 2 - swap
    tracker.bindReadBuffer(FBO_BLUR_B);
    tracker.bindDrawBuffer(FBO_BLUR_A);
    EXPECT_EQ(tracker.getReadBuffer(), FBO_BLUR_B);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_BLUR_A);
}

// Test: Restore to default framebuffer
TEST_F(OpenGLFramebufferSeparationTest, RestoreDefault_BothToZero)
{
    tracker.bindReadBuffer(FBO_SCENE);
    tracker.bindDrawBuffer(FBO_POST_PROCESS);

    // Restore to default
    tracker.bindBoth(DEFAULT_FRAMEBUFFER);

    EXPECT_EQ(tracker.getReadBuffer(), DEFAULT_FRAMEBUFFER);
    EXPECT_EQ(tracker.getDrawBuffer(), DEFAULT_FRAMEBUFFER);
    EXPECT_TRUE(tracker.isUnified());
}

// Test: Same FBO for read and draw (self-referential - edge case)
TEST_F(OpenGLFramebufferSeparationTest, SameBuffer_DefinedButMayBeInvalid)
{
    // Note: In OpenGL, reading and writing the same texture/buffer
    // can cause undefined behavior, but the state tracking should still work

    tracker.bindReadBuffer(FBO_SCENE);
    tracker.bindDrawBuffer(FBO_SCENE);

    EXPECT_EQ(tracker.getReadBuffer(), FBO_SCENE);
    EXPECT_EQ(tracker.getDrawBuffer(), FBO_SCENE);
    EXPECT_TRUE(tracker.isUnified());

    // The tracker allows this - validation is done elsewhere
}

// Stress test: Many state changes
TEST_F(OpenGLFramebufferSeparationTest, StressTest_ManyStateChanges)
{
    for (int i = 0; i < 1000; ++i) {
        uint32_t read_fbo = (i % 5) + 1;
        uint32_t draw_fbo = ((i + 1) % 5) + 1;

        tracker.bindReadBuffer(read_fbo);
        tracker.bindDrawBuffer(draw_fbo);

        ASSERT_EQ(tracker.getReadBuffer(), read_fbo);
        ASSERT_EQ(tracker.getDrawBuffer(), draw_fbo);
    }
}
