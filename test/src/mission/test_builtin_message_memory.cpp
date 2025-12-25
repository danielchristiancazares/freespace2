/**
 * FILENAME: test/src/mission/test_builtin_message_memory.cpp
 *
 * PURPOSE: Integration test for builtin_message assignment operator memory leak.
 *          Bug at missionmessage.cpp:73-84 - assignment operator doesn't free
 *          previous name before reassigning when used_strdup is true.
 *
 * TESTS ACTUAL CODE: code/mission/missionmessage.cpp
 *   - builtin_message assignment operator
 */

#include <gtest/gtest.h>
#include "util/FSTestFixture.h"

#include "mission/missionmessage.h"
#include "globalincs/pstypes.h"

class BuiltinMessageMemoryTest : public test::FSTestFixture {
public:
    BuiltinMessageMemoryTest() : test::FSTestFixture(INIT_CFILE) {
        pushModDir("mission");
    }

protected:
    void SetUp() override { test::FSTestFixture::SetUp(); }
    void TearDown() override { test::FSTestFixture::TearDown(); }
};

// ===========================================================================
// INTEGRATION TESTS - Test actual builtin_message code paths
// ===========================================================================

// Test: Assignment operator copies all fields correctly
TEST_F(BuiltinMessageMemoryTest, AssignmentOperator_CopiesAllFields)
{
    // Create source message with static name
    builtin_message src("TestMessage", 50, 3, 1000, 2, 1, 0, false);
    builtin_message dst("Other", 100, 1, 0, 0, 0, -1, false);

    dst = src;

    EXPECT_STREQ(dst.name, "TestMessage");
    EXPECT_EQ(dst.occurrence_chance, 50);
    EXPECT_EQ(dst.max_count, 3);
    EXPECT_EQ(dst.min_delay, 1000);
    EXPECT_EQ(dst.priority, 2);
    EXPECT_EQ(dst.timing, 1);
    EXPECT_EQ(dst.fallback, 0);
    EXPECT_FALSE(dst.used_strdup);
}

// Test: Assignment with strdup source creates independent copy
TEST_F(BuiltinMessageMemoryTest, AssignmentOperator_WithStrdup_CreatesCopy)
{
    // Create source with owned name
    char* owned_name = vm_strdup("OwnedName");
    builtin_message src(owned_name, 50, 3, 1000, 2, 1, 0, true);
    
    builtin_message dst("Other", 100, 1, 0, 0, 0, -1, false);
    dst = src;

    // dst should have its own copy
    EXPECT_NE(dst.name, src.name) << "Names should be different pointers";
    EXPECT_STREQ(dst.name, "OwnedName");
    EXPECT_TRUE(dst.used_strdup);
}

// BUG TEST: Repeated assignment leaks memory
// The actual code at missionmessage.cpp:73-84 doesn't free the old name
// before reassigning when used_strdup is true.
//
// This test demonstrates the leak pattern. Run under Valgrind/ASan to detect.
TEST_F(BuiltinMessageMemoryTest, AssignmentOperator_RepeatedAssignment_LeaksDemonstration)
{
    // Create messages with owned names
    char* name1 = vm_strdup("FirstName");
    char* name2 = vm_strdup("SecondName");
    char* name3 = vm_strdup("ThirdName");

    builtin_message src1(name1, 50, 1, 0, 0, 0, -1, true);
    builtin_message src2(name2, 60, 2, 0, 0, 0, -1, true);
    builtin_message src3(name3, 70, 3, 0, 0, 0, -1, true);

    builtin_message target("Initial", 0, 0, 0, 0, 0, -1, false);

    // First assignment - no leak (target didn't own memory)
    target = src1;
    EXPECT_STREQ(target.name, "FirstName");

    // Second assignment - BUG: target.name from first assignment is LEAKED
    // The fix should add at line 73:
    //   if (used_strdup && name) vm_free(const_cast<char*>(name));
    target = src2;
    EXPECT_STREQ(target.name, "SecondName");

    // Third assignment - BUG: target.name from second assignment is LEAKED
    target = src3;
    EXPECT_STREQ(target.name, "ThirdName");

    // Note: Under Valgrind/ASan, this should report 2 leaked allocations
}

// Test: Copy constructor works correctly
TEST_F(BuiltinMessageMemoryTest, CopyConstructor_CreatesIndependentCopy)
{
    char* owned_name = vm_strdup("Original");
    builtin_message src(owned_name, 50, 1, 0, 0, 0, -1, true);

    builtin_message copy(src);

    EXPECT_NE(copy.name, src.name) << "Copy should have its own name allocation";
    EXPECT_STREQ(copy.name, "Original");
    EXPECT_TRUE(copy.used_strdup);
}

// Test: Static name (no strdup) assignment is safe
TEST_F(BuiltinMessageMemoryTest, AssignmentOperator_StaticName_NoLeak)
{
    const char* static_name = "StaticName";
    builtin_message src(static_name, 50, 1, 0, 0, 0, -1, false);
    builtin_message dst("Other", 100, 1, 0, 0, 0, -1, false);

    dst = src;

    // Both should point to the same static string
    EXPECT_EQ(dst.name, static_name);
    EXPECT_FALSE(dst.used_strdup);
}

// Test: Destructor frees memory when used_strdup is true
TEST_F(BuiltinMessageMemoryTest, Destructor_WithStrdup_FreesMemory)
{
    {
        char* owned_name = vm_strdup("ToBeFreed");
        builtin_message msg(owned_name, 50, 1, 0, 0, 0, -1, true);
        // msg destructor should free owned_name when scope exits
    }
    // If we reach here without crash/leak (under Valgrind), destructor works
    SUCCEED();
}
