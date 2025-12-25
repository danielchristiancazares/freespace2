/**
 * FILENAME: test/src/object/test_shield_empty_quadrant.cpp
 *
 * PURPOSE: Integration test for shield_apply_healing empty quadrant access bug.
 *          Bug at objectshield.cpp:167 - direct access to objp->shield_quadrant[0]
 *          without checking if the vector is empty.
 *
 * TESTS ACTUAL CODE: code/object/objectshield.cpp
 *   - shield_apply_healing()
 *   - shield_get_strength()
 *   - shield_add_quad()
 */

#include <gtest/gtest.h>
#include "util/FSTestFixture.h"

#include "object/object.h"
#include "object/objectshield.h"
#include "globalincs/pstypes.h"

class ShieldEmptyQuadrantIntegrationTest : public test::FSTestFixture {
public:
    ShieldEmptyQuadrantIntegrationTest() : test::FSTestFixture(INIT_CFILE | INIT_SHIPS) {
        pushModDir("shield");
    }

protected:
    void SetUp() override { test::FSTestFixture::SetUp(); }
    void TearDown() override { test::FSTestFixture::TearDown(); }
};

// ===========================================================================
// INTEGRATION TESTS - Test actual objectshield.cpp code paths
// ===========================================================================

// BUG TEST: Calling shield_apply_healing with empty shield_quadrant vector
// The actual code at objectshield.cpp:167 does:
//   min_shield = max_shield = objp->shield_quadrant[0];
// without checking if shield_quadrant is empty first.
//
// Test is DISABLED until bug is fixed - will crash without fix.
// Remove DISABLED_ prefix after applying the fix.
TEST_F(ShieldEmptyQuadrantIntegrationTest, DISABLED_ShieldApplyHealing_EmptyQuadrant_ShouldNotCrash)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant.clear();

    // BUG: Crashes without fix at objectshield.cpp:167
    shield_apply_healing(&test_obj, 10.0f);

    EXPECT_TRUE(test_obj.shield_quadrant.empty());
}

// Positive test: Valid quadrants work correctly with ACTUAL shield functions
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldApplyHealing_ValidQuadrants_Succeeds)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant = {50.0f, 50.0f, 50.0f, 50.0f};

    float total_before = shield_get_strength(&test_obj);
    EXPECT_FLOAT_EQ(total_before, 200.0f);

    EXPECT_NO_FATAL_FAILURE(shield_apply_healing(&test_obj, 40.0f));

    float total_after = shield_get_strength(&test_obj);
    EXPECT_GT(total_after, total_before);
}

// Test: Single quadrant works
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldApplyHealing_SingleQuadrant_Works)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant = {50.0f};

    float before = test_obj.shield_quadrant[0];
    EXPECT_NO_FATAL_FAILURE(shield_apply_healing(&test_obj, 10.0f));
    EXPECT_GT(test_obj.shield_quadrant[0], before);
}

// Test: Null object pointer is handled
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldApplyHealing_NullObject_Safe)
{
    EXPECT_NO_FATAL_FAILURE(shield_apply_healing(nullptr, 10.0f));
}

// Test: shield_add_quad bounds checking
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldAddQuad_ValidQuadrant_Works)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant = {50.0f, 50.0f, 50.0f, 50.0f};

    shield_add_quad(&test_obj, 1, 25.0f);
    EXPECT_FLOAT_EQ(test_obj.shield_quadrant[1], 75.0f);
}

// Test: shield_get_strength with empty vector
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldGetStrength_EmptyQuadrant_ReturnsZero)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant.clear();

    float strength = shield_get_strength(&test_obj);
    EXPECT_FLOAT_EQ(strength, 0.0f);
}

// Test: Unequal quadrants - healing prioritizes weakest
TEST_F(ShieldEmptyQuadrantIntegrationTest, ShieldApplyHealing_UnequalQuadrants_HealsWeakest)
{
    object test_obj;
    test_obj.clear();
    test_obj.type = OBJ_SHIP;
    test_obj.shield_quadrant = {90.0f, 30.0f, 90.0f, 90.0f};

    float weakest_before = test_obj.shield_quadrant[1];
    shield_apply_healing(&test_obj, 20.0f);
    EXPECT_GT(test_obj.shield_quadrant[1], weakest_before);
}
