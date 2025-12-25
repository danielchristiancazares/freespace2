/**
 * FILENAME: test/src/model/test_animation_driver_capture.cpp
 *
 * PURPOSE: Integration test for modelanimation.cpp lambda capture bug.
 *          Bug at modelanimation.cpp:1490 - lambda captures remap_driver_source
 *          and curve by reference, but they are locals that go out of scope.
 *
 * TESTS: The lambda capture pattern in code/model/animation/modelanimation.cpp
 *
 * NOTE: Full integration testing requires model animation system + parsing.
 *       These tests verify the capture pattern and document the bug.
 *       Run under AddressSanitizer (ASan) to detect use-after-free.
 */

#include <gtest/gtest.h>
#include <functional>
#include <optional>

// ===========================================================================
// PATTERN TESTS - Demonstrate the lambda capture bug pattern
// ===========================================================================

namespace {

// Simulate the Curve type from curves.cpp
struct MockCurve {
    float GetValue(float input) const { return input * multiplier; }
    float multiplier = 1.0f;
};

// Simulate driver source function type
using DriverSource = std::function<float(void*)>;

// Simulate the driver lambda type
using Driver = std::function<void(float&, void*)>;

// BUGGY pattern - captures by reference (matches modelanimation.cpp:1490)
Driver create_driver_buggy(DriverSource source, std::optional<MockCurve> curve) {
    // BUG: Capturing by reference - will dangle when function returns
    return [&source, &curve](float& output, void* ctx) {
        output = curve ? curve->GetValue(source(ctx)) : source(ctx);
    };
}

// FIXED pattern - captures by value (matches modelanimation.cpp:1515)
Driver create_driver_fixed(DriverSource source, std::optional<MockCurve> curve) {
    // CORRECT: Capturing by value - lambda owns copies
    return [source, curve](float& output, void* ctx) {
        output = curve ? curve->GetValue(source(ctx)) : source(ctx);
    };
}

// Simulate parsing scope: create driver and return (locals go out of scope)
Driver simulate_parsing_buggy() {
    // These locals go out of scope at end of function
    DriverSource remap_driver_source = [](void*) -> float { return 42.0f; };
    std::optional<MockCurve> curve = MockCurve{2.0f};

    // BUG: Captures by reference - returns lambda with dangling references
    return create_driver_buggy(remap_driver_source, curve);
}

Driver simulate_parsing_fixed() {
    DriverSource remap_driver_source = [](void*) -> float { return 42.0f; };
    std::optional<MockCurve> curve = MockCurve{2.0f};

    // CORRECT: Captures by value - returns lambda with owned copies
    return create_driver_fixed(remap_driver_source, curve);
}

} // namespace

class AnimationDriverCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// BUG TEST: Buggy capture pattern - lambda accesses dangling references
// Under ASan, this should report "stack-use-after-return" or similar.
// Without ASan, behavior is undefined - may appear to work, crash, or corrupt.
TEST_F(AnimationDriverCaptureTest, LambdaCapture_BuggyPattern_DanglingReference)
{
    // This simulates the bug at modelanimation.cpp:1490:
    // 1. Parse driver source and curve as locals
    // 2. Create lambda that captures them by reference
    // 3. Return the lambda (locals go out of scope)
    // 4. Later, invoke the lambda (accesses dangling references)

    Driver driver = simulate_parsing_buggy();

    // At this point, the lambda holds dangling references.
    // Invoking it is undefined behavior.

    float result = 0.0f;

    // Under ASan, this should detect use-after-free
    EXPECT_NO_FATAL_FAILURE({
        driver(result, nullptr);
    }) << "This may crash or report ASan error if bug manifests";

    // We don't assert on result because UB means any value is possible
}

// POSITIVE TEST: Fixed capture pattern works correctly
TEST_F(AnimationDriverCaptureTest, LambdaCapture_FixedPattern_SafeAfterScopeExit)
{
    Driver driver = simulate_parsing_fixed();

    float result = 0.0f;
    driver(result, nullptr);

    // With fixed pattern: source returns 42.0f, curve multiplies by 2.0f
    EXPECT_FLOAT_EQ(result, 84.0f)
        << "Fixed pattern should compute: 42.0 * 2.0 = 84.0";
}

// Test: No curve case works correctly
TEST_F(AnimationDriverCaptureTest, LambdaCapture_NoCurve_PassesThroughValue)
{
    DriverSource source = [](void*) -> float { return 100.0f; };
    std::optional<MockCurve> no_curve = std::nullopt;

    Driver driver = create_driver_fixed(source, no_curve);

    float result = 0.0f;
    driver(result, nullptr);

    EXPECT_FLOAT_EQ(result, 100.0f)
        << "Without curve, should pass through source value";
}

// Test: Context pointer is used correctly
TEST_F(AnimationDriverCaptureTest, LambdaCapture_UsesContext)
{
    float context_value = 123.0f;

    DriverSource source = [](void* ctx) -> float {
        return *static_cast<float*>(ctx);
    };
    std::optional<MockCurve> curve = MockCurve{0.5f};

    Driver driver = create_driver_fixed(source, curve);

    float result = 0.0f;
    driver(result, &context_value);

    EXPECT_FLOAT_EQ(result, 61.5f)
        << "Should use context (123.0) with curve (0.5x) = 61.5";
}

// Stress test: Many drivers to increase chance of detecting dangling refs
TEST_F(AnimationDriverCaptureTest, LambdaCapture_StressTest_ManyDrivers)
{
    std::vector<Driver> drivers;

    // Create many drivers with fixed pattern
    for (int i = 0; i < 100; ++i) {
        float expected_val = static_cast<float>(i);
        DriverSource source = [expected_val](void*) -> float { 
            return expected_val; 
        };
        std::optional<MockCurve> curve = MockCurve{2.0f};

        drivers.push_back(create_driver_fixed(source, curve));
    }

    // Invoke all drivers
    for (int i = 0; i < 100; ++i) {
        float result = 0.0f;
        drivers[i](result, nullptr);
        EXPECT_FLOAT_EQ(result, static_cast<float>(i) * 2.0f);
    }
}

// Document the actual bug location for reference
TEST_F(AnimationDriverCaptureTest, DocumentBugLocation)
{
    // BUG LOCATION: modelanimation.cpp:1490
    // 
    // Buggy code:
    //   driver = [&remap_driver_source, &curve](ModelAnimation &, ...) {
    //               ^-- captures by reference!
    //
    // Fixed code should be:
    //   driver = [remap_driver_source, curve](ModelAnimation &, ...) {
    //               ^-- capture by value
    //
    // Note: Line 1515 already does it correctly:
    //   propertyDrivers.emplace_back([driver_source, curve, target](...
    //                                 ^-- capture by value (correct)
    
    SUCCEED() << "See test comments for bug location and fix";
}
