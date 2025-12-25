/**
 * FILENAME: test/src/ship/test_ship_subsys_index_object_type.cpp
 *
 * PURPOSE: Integration test for ship_get_subsys_index object type verification bug.
 *          Bug at ship.cpp:15834 - The code assumes Objects[parent_objnum] is type
 *          OBJ_SHIP without verification before accessing .instance as a ship index.
 *
 * TESTS ACTUAL CODE: code/ship/ship.cpp
 *   - ship_get_subsys_index()
 */

#include <gtest/gtest.h>
#include "util/FSTestFixture.h"

#include "object/object.h"
#include "ship/ship.h"
#include "globalincs/pstypes.h"

class ShipSubsysIndexTest : public test::FSTestFixture {
public:
    ShipSubsysIndexTest() : test::FSTestFixture(INIT_CFILE | INIT_SHIPS) {
        pushModDir("ship");
    }

protected:
    void SetUp() override { test::FSTestFixture::SetUp(); }
    void TearDown() override { test::FSTestFixture::TearDown(); }
};

// ===========================================================================
// INTEGRATION TESTS - Test actual ship.cpp code paths
// ===========================================================================

// Test: Null subsys pointer is handled
TEST_F(ShipSubsysIndexTest, ShipGetSubsysIndex_NullSubsys_ReturnsNegative)
{
    // ship_get_subsys_index checks for nullptr at line 15827
    int result = ship_get_subsys_index(nullptr);
    EXPECT_EQ(result, -1);
}

// Test: Negative parent_objnum is handled
TEST_F(ShipSubsysIndexTest, ShipGetSubsysIndex_NegativeParentObjnum_ReturnsNegative)
{
    // Create a subsys with invalid parent
    ship_subsys subsys;
    memset(&subsys, 0, sizeof(subsys));
    subsys.parent_objnum = -1;

    int result = ship_get_subsys_index(&subsys);
    EXPECT_EQ(result, -1);
}

// BUG TEST: Parent object is not a ship
// The actual code at ship.cpp:15834 does:
//   auto sp = &Ships[Objects[subsys->parent_objnum].instance];
// without verifying Objects[parent_objnum].type == OBJ_SHIP
//
// Test is DISABLED until bug is fixed - accessing wrong object type is UB.
// Remove DISABLED_ prefix after applying the fix.
TEST_F(ShipSubsysIndexTest, DISABLED_ShipGetSubsysIndex_ParentNotShip_ShouldReturnNegative)
{
    // Create an object that is NOT a ship (e.g., debris, weapon)
    // This would require object system to be properly initialized
    // and an object of non-ship type to be created.
    //
    // The fix should add:
    //   if (Objects[subsys->parent_objnum].type != OBJ_SHIP)
    //       return -1;
    
    // For now, this test documents the bug exists
    FAIL() << "This test requires proper object system setup to create non-ship objects";
}
