/**
 * FILENAME: test/src/math/test_vecmat_matrix_uvec_norm.cpp
 *
 * PURPOSE: Validate vm_vector_2_matrix_uvec_norm correctly handles the case where
 *          uvec is null and both fvec and rvec are provided. Bug: xvec is never
 *          set from the provided rvec, instead using identity (1,0,0).
 *
 * DEPENDENCIES:
 * - math/vecmat.h
 * - gtest
 *
 * KEY TESTS:
 * 1. UvecNull_FvecAndRvecProvided_RvecUsed: Validates rvec is actually used when
 *    uvec is null but both fvec and rvec are provided
 * 2. UvecProvided_RvecProvided_UsesRvecBranch: Validates the uvec+rvec branch works
 */

#include <gtest/gtest.h>
#include <math/vecmat.h>

namespace {

// Helper to check if two vectors are approximately equal
bool vec_near(const vec3d& a, const vec3d& b, float epsilon = 0.001f) {
    return std::abs(a.xyz.x - b.xyz.x) < epsilon &&
           std::abs(a.xyz.y - b.xyz.y) < epsilon &&
           std::abs(a.xyz.z - b.xyz.z) < epsilon;
}

// Helper to check orthogonality of matrix
bool is_orthogonal(const matrix& m, float epsilon = 0.001f) {
    // Check each pair of basis vectors is perpendicular
    float dot_rv_uv = vm_vec_dot(&m.vec.rvec, &m.vec.uvec);
    float dot_uv_fv = vm_vec_dot(&m.vec.uvec, &m.vec.fvec);
    float dot_rv_fv = vm_vec_dot(&m.vec.rvec, &m.vec.fvec);

    return std::abs(dot_rv_uv) < epsilon &&
           std::abs(dot_uv_fv) < epsilon &&
           std::abs(dot_rv_fv) < epsilon;
}

// Helper to check vectors are normalized
bool is_normalized(const vec3d& v, float epsilon = 0.001f) {
    float mag = vm_vec_mag(&v);
    return std::abs(mag - 1.0f) < epsilon;
}

} // namespace

class VecmatMatrixUvecNormTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// BUG REGRESSION TEST: When uvec is null and both fvec+rvec are provided,
// the code enters the `else if (fvec != nullptr)` branch at line 972,
// then the `if (rvec != nullptr)` branch at line 975, but never assigns
// xvec from the provided rvec. Line 976 uses xvec in cross product but
// xvec still has identity value (1,0,0) from line 939.
TEST_F(VecmatMatrixUvecNormTest, UvecNull_FvecAndRvecProvided_RvecShouldInfluenceResult)
{
    // Provide fvec and rvec that are NOT axis-aligned to detect the bug
    // Bug manifests when: provided rvec != identity rvec (1,0,0)
    vec3d fvec = {{{0.0f, 0.0f, 1.0f}}};  // normalized +Z
    vec3d rvec = {{{0.0f, 1.0f, 0.0f}}};  // normalized +Y (NOT the identity rvec)

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, &fvec, nullptr, &rvec);

    // The resulting matrix should:
    // 1. Have fvec as the forward vector
    EXPECT_TRUE(vec_near(result.vec.fvec, fvec))
        << "Forward vector should match provided fvec. "
        << "Expected (" << fvec.xyz.x << "," << fvec.xyz.y << "," << fvec.xyz.z << ") "
        << "Got (" << result.vec.fvec.xyz.x << "," << result.vec.fvec.xyz.y << "," << result.vec.fvec.xyz.z << ")";

    // 2. Be orthogonal
    EXPECT_TRUE(is_orthogonal(result))
        << "Matrix should be orthogonal. Dot products: "
        << "rvec.uvec=" << vm_vec_dot(&result.vec.rvec, &result.vec.uvec) << ", "
        << "uvec.fvec=" << vm_vec_dot(&result.vec.uvec, &result.vec.fvec) << ", "
        << "rvec.fvec=" << vm_vec_dot(&result.vec.rvec, &result.vec.fvec);

    // 3. The rvec should be influenced by the provided rvec, not be identity (1,0,0)
    // If the bug exists, result.vec.rvec will be computed from cross product of
    // uvec x fvec, but uvec was computed from fvec x (1,0,0) instead of fvec x rvec

    // With fvec = (0,0,1) and rvec = (0,1,0):
    // uvec should be computed from fvec cross xvec (where xvec should be from rvec)
    // But if bug exists, xvec=(1,0,0), giving uvec = (0,0,1) x (1,0,0) = (0,1,0)
    // vs correct: xvec=(0,1,0), giving uvec = (0,0,1) x (0,1,0) = (-1,0,0)

    vec3d buggy_uvec = {{{0.0f, 1.0f, 0.0f}}};
    vec3d correct_uvec = {{{-1.0f, 0.0f, 0.0f}}};

    // This assertion will fail if the bug exists
    EXPECT_FALSE(vec_near(result.vec.uvec, buggy_uvec))
        << "BUG DETECTED: uvec was computed using identity rvec (1,0,0) instead of provided rvec. "
        << "Got uvec (" << result.vec.uvec.xyz.x << "," << result.vec.uvec.xyz.y << "," << result.vec.uvec.xyz.z << ") "
        << "which matches buggy computation, not provided rvec-based computation.";
}

// Positive test: verify uvec-primary branch with rvec works correctly
TEST_F(VecmatMatrixUvecNormTest, UvecProvided_FvecProvided_ProducesValidMatrix)
{
    vec3d uvec = {{{0.0f, 1.0f, 0.0f}}};  // +Y
    vec3d fvec = {{{0.0f, 0.0f, 1.0f}}};  // +Z

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, &fvec, &uvec, nullptr);

    EXPECT_TRUE(vec_near(result.vec.uvec, uvec));
    EXPECT_TRUE(is_orthogonal(result));
    EXPECT_TRUE(is_normalized(result.vec.rvec));
    EXPECT_TRUE(is_normalized(result.vec.uvec));
    EXPECT_TRUE(is_normalized(result.vec.fvec));
}

// Positive test: uvec only, should generate valid orthogonal matrix
TEST_F(VecmatMatrixUvecNormTest, UvecOnly_GeneratesOrthogonalMatrix)
{
    vec3d uvec = {{{0.0f, 1.0f, 0.0f}}};  // +Y

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, nullptr, &uvec, nullptr);

    EXPECT_TRUE(vec_near(result.vec.uvec, uvec));
    EXPECT_TRUE(is_orthogonal(result));
}

// Positive test: uvec+rvec branch (line 960-970)
TEST_F(VecmatMatrixUvecNormTest, UvecProvided_RvecProvided_UsesRvecBranch)
{
    vec3d uvec = {{{0.0f, 1.0f, 0.0f}}};  // +Y
    vec3d rvec = {{{1.0f, 0.0f, 0.0f}}};  // +X

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, nullptr, &uvec, &rvec);

    EXPECT_TRUE(vec_near(result.vec.uvec, uvec));
    EXPECT_TRUE(is_orthogonal(result));

    // With uvec=+Y and rvec=+X, fvec should be -Z (cross product Y x X = -Z)
    vec3d expected_fvec = {{{0.0f, 0.0f, -1.0f}}};
    EXPECT_TRUE(vec_near(result.vec.fvec, expected_fvec))
        << "fvec should be uvec cross rvec = -Z";
}

// Edge case: parallel vectors should fall back gracefully
TEST_F(VecmatMatrixUvecNormTest, FvecOnly_GeneratesOrthogonalMatrix)
{
    vec3d fvec = {{{0.0f, 0.0f, 1.0f}}};  // +Z

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, &fvec, nullptr, nullptr);

    // Should hit the fvec-only branch and generate vectors
    EXPECT_TRUE(vec_near(result.vec.fvec, fvec));
    EXPECT_TRUE(is_orthogonal(result));
}

// Stress test with non-axis-aligned vectors
TEST_F(VecmatMatrixUvecNormTest, NonAxisAligned_ProducesOrthogonalMatrix)
{
    // 45-degree rotated vectors
    vec3d fvec = {{{0.707107f, 0.0f, 0.707107f}}};  // normalized (1,0,1)
    vec3d uvec = {{{0.0f, 1.0f, 0.0f}}};             // +Y

    matrix result;
    vm_vector_2_matrix_uvec_norm(&result, &fvec, &uvec, nullptr);

    EXPECT_TRUE(is_orthogonal(result))
        << "Matrix should remain orthogonal with non-axis-aligned inputs";
    EXPECT_TRUE(is_normalized(result.vec.rvec));
    EXPECT_TRUE(is_normalized(result.vec.uvec));
    EXPECT_TRUE(is_normalized(result.vec.fvec));
}
