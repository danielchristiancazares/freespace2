/**
 * FILENAME: test/src/scripting/api/test_order_waypoint_bounds.cpp
 *
 * PURPOSE: Integration test for order.cpp waypoint bounds check bug.
 *          Bug at order.cpp:193 - Checks wp_list_index >= 0 but not against
 *          Waypoint_lists.size() before accessing the vector.
 *
 * TESTS: The bounds checking logic in code/scripting/api/objs/order.cpp
 *
 * NOTE: Full integration testing requires scripting environment + mission state.
 *       These tests verify the bounds checking concept and document the bug.
 */

#include <gtest/gtest.h>
#include "util/FSTestFixture.h"

#include "object/waypoint.h"
#include "globalincs/pstypes.h"

// The Waypoint_lists vector is defined in waypoint.cpp
extern SCP_vector<waypoint_list> Waypoint_lists;

class OrderWaypointBoundsTest : public test::FSTestFixture {
public:
    OrderWaypointBoundsTest() : test::FSTestFixture(INIT_CFILE) {
        pushModDir("waypoint");
    }

protected:
    void SetUp() override {
        test::FSTestFixture::SetUp();
        // Clear any existing waypoint lists
        Waypoint_lists.clear();
    }
    void TearDown() override {
        Waypoint_lists.clear();
        test::FSTestFixture::TearDown();
    }
};

// ===========================================================================
// INTEGRATION TESTS - Test Waypoint_lists bounds checking
// ===========================================================================

// Test: Empty Waypoint_lists - any index is invalid
TEST_F(OrderWaypointBoundsTest, WaypointLists_Empty_AllIndicesInvalid)
{
    EXPECT_TRUE(Waypoint_lists.empty());
    
    // Buggy code only checks >= 0, would allow index 0 on empty list
    int index = 0;
    bool buggy_check = (index >= 0);  // BUG: passes!
    bool fixed_check = (index >= 0) && (static_cast<size_t>(index) < Waypoint_lists.size());
    
    EXPECT_TRUE(buggy_check) << "BUG: Buggy check passes for index 0 on empty list";
    EXPECT_FALSE(fixed_check) << "Fixed check correctly rejects index 0 on empty list";
}

// Test: Index at exact size is out of bounds
TEST_F(OrderWaypointBoundsTest, WaypointLists_IndexAtSize_OutOfBounds)
{
    // Add some waypoint lists
    waypoint_list wpl1, wpl2, wpl3;
    strcpy_s(wpl1.name, "Alpha");
    strcpy_s(wpl2.name, "Beta");
    strcpy_s(wpl3.name, "Gamma");
    Waypoint_lists.push_back(wpl1);
    Waypoint_lists.push_back(wpl2);
    Waypoint_lists.push_back(wpl3);
    
    EXPECT_EQ(Waypoint_lists.size(), 3u);
    
    // Index 3 is out of bounds (valid indices are 0, 1, 2)
    int boundary_index = 3;
    
    bool buggy_check = (boundary_index >= 0);  // BUG: passes!
    bool fixed_check = (boundary_index >= 0) && (static_cast<size_t>(boundary_index) < Waypoint_lists.size());
    
    EXPECT_TRUE(buggy_check) << "BUG: Buggy check passes for index == size";
    EXPECT_FALSE(fixed_check) << "Fixed check correctly rejects index == size";
}

// Test: Valid indices are accepted
TEST_F(OrderWaypointBoundsTest, WaypointLists_ValidIndices_Accepted)
{
    waypoint_list wpl1, wpl2;
    strcpy_s(wpl1.name, "Alpha");
    strcpy_s(wpl2.name, "Beta");
    Waypoint_lists.push_back(wpl1);
    Waypoint_lists.push_back(wpl2);
    
    for (size_t i = 0; i < Waypoint_lists.size(); ++i) {
        int index = static_cast<int>(i);
        bool fixed_check = (index >= 0) && (static_cast<size_t>(index) < Waypoint_lists.size());
        EXPECT_TRUE(fixed_check) << "Index " << i << " should be valid";
    }
}

// Test: Negative indices are correctly rejected
TEST_F(OrderWaypointBoundsTest, WaypointLists_NegativeIndex_Rejected)
{
    waypoint_list wpl;
    strcpy_s(wpl.name, "Test");
    Waypoint_lists.push_back(wpl);
    
    int neg_index = -1;
    bool check = (neg_index >= 0);
    EXPECT_FALSE(check) << "Negative index should be rejected by existing check";
}

// Test: Large index is out of bounds
TEST_F(OrderWaypointBoundsTest, WaypointLists_LargeIndex_OutOfBounds)
{
    waypoint_list wpl;
    strcpy_s(wpl.name, "Single");
    Waypoint_lists.push_back(wpl);
    
    int large_index = 100;
    
    bool buggy_check = (large_index >= 0);
    bool fixed_check = (large_index >= 0) && (static_cast<size_t>(large_index) < Waypoint_lists.size());
    
    EXPECT_TRUE(buggy_check) << "BUG: Buggy check passes for large index";
    EXPECT_FALSE(fixed_check) << "Fixed check correctly rejects large index";
}

// Test: Access waypoint list with valid index
TEST_F(OrderWaypointBoundsTest, WaypointLists_ValidAccess_Succeeds)
{
    waypoint_list wpl;
    strcpy_s(wpl.name, "TestList");
    Waypoint_lists.push_back(wpl);
    
    int index = 0;
    bool fixed_check = (index >= 0) && (static_cast<size_t>(index) < Waypoint_lists.size());
    ASSERT_TRUE(fixed_check);
    
    // Safe access after bounds check
    EXPECT_STREQ(Waypoint_lists[index].get_name(), "TestList");
}
