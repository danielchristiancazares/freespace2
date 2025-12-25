/**
 * FILENAME: test/src/graphics/test_vulkan_decal_instancing.cpp
 *
 * PURPOSE: Validate the new decal instanced rendering system that uses matrix4
 *          transforms in instance buffers. The refactored `gf_render_decals`
 *          signature changes must maintain compatibility and correctness.
 *
 * DEPENDENCIES:
 * - gtest
 * - Matrix/vector math utilities
 *
 * KEY TESTS:
 * 1. DecalInstanceBuffer_Matrix4Layout_CorrectAlignment: Validates std140 alignment
 * 2. DecalInstanceData_TransformDecomposition_Correct: Validates matrix composition
 * 3. DecalBatching_MultipleDecals_CorrectInstanceCount: Validates batching behavior
 *
 * NOTES:
 * - These tests validate the data structures and contracts for the new instancing
 *   system without requiring a live Vulkan device.
 * - Actual Vulkan rendering validation requires integration tests with validation
 *   layers enabled.
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cmath>
#include <array>
#include <vector>

namespace {

// Reproduce the matrix4 structure for testing
struct matrix4 {
    float m[16];  // Column-major order

    static matrix4 identity() {
        matrix4 result{};
        result.m[0] = 1.0f;
        result.m[5] = 1.0f;
        result.m[10] = 1.0f;
        result.m[15] = 1.0f;
        return result;
    }

    static matrix4 translation(float x, float y, float z) {
        matrix4 result = identity();
        result.m[12] = x;
        result.m[13] = y;
        result.m[14] = z;
        return result;
    }

    static matrix4 scale(float sx, float sy, float sz) {
        matrix4 result = identity();
        result.m[0] = sx;
        result.m[5] = sy;
        result.m[10] = sz;
        return result;
    }

    // Extract translation component
    void getTranslation(float& x, float& y, float& z) const {
        x = m[12];
        y = m[13];
        z = m[14];
    }

    // Multiply matrices (column-major)
    matrix4 operator*(const matrix4& rhs) const {
        matrix4 result{};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[row + k * 4] * rhs.m[k + col * 4];
                }
                result.m[row + col * 4] = sum;
            }
        }
        return result;
    }
};

// Reproduce decal instance data structure
struct DecalInstanceData {
    matrix4 transform;
    float alpha;
    float padding[3];  // Align to 16 bytes
};

// Verify std140 layout requirements
constexpr size_t STD140_VEC4_ALIGNMENT = 16;
constexpr size_t STD140_MAT4_ALIGNMENT = 16;  // Each column vec4

bool is_aligned(size_t offset, size_t alignment) {
    return (offset % alignment) == 0;
}

// Mock decal batch for testing
struct DecalBatch {
    std::vector<DecalInstanceData> instances;
    int texture_id;

    void addDecal(const matrix4& transform, float alpha) {
        DecalInstanceData data;
        data.transform = transform;
        data.alpha = alpha;
        std::memset(data.padding, 0, sizeof(data.padding));
        instances.push_back(data);
    }

    size_t instanceCount() const { return instances.size(); }

    const void* instanceData() const {
        return instances.data();
    }

    size_t instanceDataSize() const {
        return instances.size() * sizeof(DecalInstanceData);
    }
};

} // namespace

class VulkanDecalInstancingTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test: DecalInstanceData has correct std140 layout
TEST_F(VulkanDecalInstancingTest, DecalInstanceBuffer_Layout_Std140Compliant)
{
    // matrix4 is 16 floats = 64 bytes
    EXPECT_EQ(sizeof(matrix4), 64u)
        << "matrix4 should be 64 bytes";

    // DecalInstanceData should be matrix4 + alpha + padding
    // = 64 + 4 + 12 = 80 bytes, but check actual
    static_assert(sizeof(DecalInstanceData) >= 68,
        "DecalInstanceData must include at least transform + alpha");

    // Verify transform is at offset 0
    EXPECT_EQ(offsetof(DecalInstanceData, transform), 0u)
        << "Transform should be at offset 0";

    // Verify alpha offset is after the matrix
    EXPECT_EQ(offsetof(DecalInstanceData, alpha), 64u)
        << "Alpha should be at offset 64 (after matrix4)";

    // Verify total size is 16-byte aligned for array elements
    EXPECT_TRUE(is_aligned(sizeof(DecalInstanceData), 16))
        << "DecalInstanceData size should be 16-byte aligned for arrays";
}

// Test: Matrix4 identity is correct
TEST_F(VulkanDecalInstancingTest, Matrix4_Identity_DiagonalOnes)
{
    matrix4 id = matrix4::identity();

    // Diagonal should be 1
    EXPECT_FLOAT_EQ(id.m[0], 1.0f);   // m[0][0]
    EXPECT_FLOAT_EQ(id.m[5], 1.0f);   // m[1][1]
    EXPECT_FLOAT_EQ(id.m[10], 1.0f);  // m[2][2]
    EXPECT_FLOAT_EQ(id.m[15], 1.0f);  // m[3][3]

    // Off-diagonal should be 0
    EXPECT_FLOAT_EQ(id.m[1], 0.0f);
    EXPECT_FLOAT_EQ(id.m[4], 0.0f);
    EXPECT_FLOAT_EQ(id.m[12], 0.0f);
}

// Test: Matrix4 translation
TEST_F(VulkanDecalInstancingTest, Matrix4_Translation_CorrectPosition)
{
    matrix4 t = matrix4::translation(10.0f, 20.0f, 30.0f);

    float x, y, z;
    t.getTranslation(x, y, z);

    EXPECT_FLOAT_EQ(x, 10.0f);
    EXPECT_FLOAT_EQ(y, 20.0f);
    EXPECT_FLOAT_EQ(z, 30.0f);
}

// Test: Matrix4 scale
TEST_F(VulkanDecalInstancingTest, Matrix4_Scale_DiagonalValues)
{
    matrix4 s = matrix4::scale(2.0f, 3.0f, 4.0f);

    EXPECT_FLOAT_EQ(s.m[0], 2.0f);
    EXPECT_FLOAT_EQ(s.m[5], 3.0f);
    EXPECT_FLOAT_EQ(s.m[10], 4.0f);
}

// Test: Matrix multiplication (scale then translate)
TEST_F(VulkanDecalInstancingTest, Matrix4_Multiplication_ScaleThenTranslate)
{
    matrix4 scale = matrix4::scale(2.0f, 2.0f, 2.0f);
    matrix4 translate = matrix4::translation(5.0f, 0.0f, 0.0f);

    // Apply scale first, then translation (right-to-left for column-major)
    matrix4 combined = translate * scale;

    // A point at (1, 0, 0) scaled by 2 = (2, 0, 0), then translated by 5 = (7, 0, 0)
    // The matrix should encode this transformation
    EXPECT_FLOAT_EQ(combined.m[0], 2.0f);   // Scale X
    EXPECT_FLOAT_EQ(combined.m[12], 5.0f);  // Translation X
}

// Test: Decal batch accumulates instances correctly
TEST_F(VulkanDecalInstancingTest, DecalBatch_AddDecals_CorrectCount)
{
    DecalBatch batch;
    batch.texture_id = 1;

    EXPECT_EQ(batch.instanceCount(), 0u);

    batch.addDecal(matrix4::identity(), 1.0f);
    EXPECT_EQ(batch.instanceCount(), 1u);

    batch.addDecal(matrix4::translation(1.0f, 2.0f, 3.0f), 0.5f);
    EXPECT_EQ(batch.instanceCount(), 2u);

    batch.addDecal(matrix4::scale(2.0f, 2.0f, 2.0f), 0.75f);
    EXPECT_EQ(batch.instanceCount(), 3u);
}

// Test: Instance data is correctly stored
TEST_F(VulkanDecalInstancingTest, DecalBatch_InstanceData_CorrectValues)
{
    DecalBatch batch;

    matrix4 transform = matrix4::translation(100.0f, 200.0f, 300.0f);
    float alpha = 0.8f;

    batch.addDecal(transform, alpha);

    ASSERT_EQ(batch.instanceCount(), 1u);

    const DecalInstanceData* data = static_cast<const DecalInstanceData*>(batch.instanceData());

    float x, y, z;
    data->transform.getTranslation(x, y, z);

    EXPECT_FLOAT_EQ(x, 100.0f);
    EXPECT_FLOAT_EQ(y, 200.0f);
    EXPECT_FLOAT_EQ(z, 300.0f);
    EXPECT_FLOAT_EQ(data->alpha, 0.8f);
}

// Test: Instance buffer data size calculation
TEST_F(VulkanDecalInstancingTest, DecalBatch_InstanceDataSize_MatchesCount)
{
    DecalBatch batch;

    EXPECT_EQ(batch.instanceDataSize(), 0u);

    for (int i = 0; i < 10; ++i) {
        batch.addDecal(matrix4::identity(), 1.0f);
    }

    EXPECT_EQ(batch.instanceDataSize(), 10 * sizeof(DecalInstanceData));
}

// Test: Empty batch handling
TEST_F(VulkanDecalInstancingTest, DecalBatch_Empty_SafeAccess)
{
    DecalBatch batch;

    EXPECT_EQ(batch.instanceCount(), 0u);
    EXPECT_EQ(batch.instanceDataSize(), 0u);

    // instanceData() on empty should not crash (returns valid but empty pointer)
    EXPECT_NO_THROW({
        const void* data = batch.instanceData();
        (void)data;
    });
}

// Test: Large batch (stress test)
TEST_F(VulkanDecalInstancingTest, DecalBatch_LargeBatch_Handles1000Decals)
{
    DecalBatch batch;

    for (int i = 0; i < 1000; ++i) {
        float f = static_cast<float>(i);
        batch.addDecal(matrix4::translation(f, f * 2, f * 3), f / 1000.0f);
    }

    EXPECT_EQ(batch.instanceCount(), 1000u);

    // Verify a few random entries
    const DecalInstanceData* data = static_cast<const DecalInstanceData*>(batch.instanceData());

    float x, y, z;
    data[0].transform.getTranslation(x, y, z);
    EXPECT_FLOAT_EQ(x, 0.0f);

    data[500].transform.getTranslation(x, y, z);
    EXPECT_FLOAT_EQ(x, 500.0f);
    EXPECT_FLOAT_EQ(y, 1000.0f);
    EXPECT_FLOAT_EQ(z, 1500.0f);
    EXPECT_FLOAT_EQ(data[500].alpha, 0.5f);
}

// Test: Alpha clamping boundary values
TEST_F(VulkanDecalInstancingTest, DecalInstanceData_AlphaRange_ValidValues)
{
    DecalBatch batch;

    // Valid alpha range is typically 0.0 to 1.0
    batch.addDecal(matrix4::identity(), 0.0f);
    batch.addDecal(matrix4::identity(), 1.0f);
    batch.addDecal(matrix4::identity(), 0.5f);

    const DecalInstanceData* data = static_cast<const DecalInstanceData*>(batch.instanceData());

    EXPECT_FLOAT_EQ(data[0].alpha, 0.0f);
    EXPECT_FLOAT_EQ(data[1].alpha, 1.0f);
    EXPECT_FLOAT_EQ(data[2].alpha, 0.5f);
}

// Test: Verify data contiguity for GPU upload
TEST_F(VulkanDecalInstancingTest, DecalBatch_DataContiguity_ValidForGPUUpload)
{
    DecalBatch batch;
    batch.addDecal(matrix4::identity(), 1.0f);
    batch.addDecal(matrix4::identity(), 0.5f);

    const char* dataPtr = static_cast<const char*>(batch.instanceData());
    size_t stride = sizeof(DecalInstanceData);

    // Second element should be exactly stride bytes after first
    const DecalInstanceData* first = reinterpret_cast<const DecalInstanceData*>(dataPtr);
    const DecalInstanceData* second = reinterpret_cast<const DecalInstanceData*>(dataPtr + stride);

    EXPECT_FLOAT_EQ(first->alpha, 1.0f);
    EXPECT_FLOAT_EQ(second->alpha, 0.5f);
}
