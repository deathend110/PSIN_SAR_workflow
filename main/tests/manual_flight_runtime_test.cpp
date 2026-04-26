#include "workflow/infer/manual_flight_runtime.hpp"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "manual_flight_runtime_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            Fail(message);
        }
    }

    void ExpectContains(const std::string &text, const std::string &needle, const std::string &message)
    {
        if (text.find(needle) == std::string::npos)
        {
            Fail(message + " actual=" + text);
        }
    }

    void ExpectResetLikeDefaults(const workflow::infer::ManualFlightTelemetry &telemetry)
    {
        Expect(!telemetry.active, "reset/default state should be inactive");
        Expect(!telemetry.paused, "reset/default state should clear paused");
        Expect(!telemetry.edge_blocked, "reset/default state should clear edge_blocked");
        Expect(telemetry.patch_count == 0, "reset/default state should clear patch_count");
        Expect(telemetry.path_points == 0, "reset/default state should clear path_points");
        Expect(telemetry.current_direction == "right", "reset/default state should restore current direction");
        Expect(telemetry.pending_direction == "right", "reset/default state should restore pending direction");
        Expect(telemetry.position_x == workflow::infer::kExpectedH / 2, "reset/default state should restore center x");
        Expect(telemetry.position_y == workflow::infer::kExpectedH / 2, "reset/default state should restore center y");
    }

    void TestSequentialInstancesDoNotLeakState()
    {
        workflow::infer::ManualFlightRuntimeState first;
        first.configure(workflow::infer::ManualFlightSettings{});
        first.activate(1024, 1024, 512, 256);

        cv::Point center;
        Expect(first.waitNextCenter([] { return false; }, center), "first runtime should yield initial center");
        first.markInferenceCommitted(center);

        auto telemetry = first.telemetry();
        Expect(telemetry.active, "first runtime should be active");
        Expect(telemetry.patch_count == 1, "first runtime should record one committed patch");
        Expect(telemetry.path_points >= 1, "first runtime should record path points");

        first.reset();
        ExpectResetLikeDefaults(first.telemetry());

        workflow::infer::ManualFlightRuntimeState second;
        second.configure(workflow::infer::ManualFlightSettings{});
        second.activate(1024, 1024, 512, 256);
        telemetry = second.telemetry();
        Expect(telemetry.active, "second runtime should activate independently");
        Expect(telemetry.patch_count == 0, "second runtime should start with zero patch_count");
        Expect(telemetry.path_points == 1, "second runtime should start with a single path point");
        Expect(telemetry.current_direction == "right", "second runtime should start facing right");
        Expect(telemetry.pending_direction == "right", "second runtime should start pending right");
        Expect(!telemetry.edge_blocked, "second runtime should not inherit edge_blocked");
    }

    void TestInactiveSubmitIsRejected()
    {
        workflow::infer::ManualFlightRuntimeState runtime;
        runtime.configure(workflow::infer::ManualFlightSettings{});
        std::string message;
        Expect(!runtime.submitDirectionKey("w", true, &message), "inactive runtime must reject direction submit");
        ExpectContains(message, "not active", "inactive submit should report not active");
    }

    void TestDuplicateDirectionSubmitIsNoOp()
    {
        workflow::infer::ManualFlightRuntimeState runtime;
        runtime.configure(workflow::infer::ManualFlightSettings{});
        runtime.activate(1024, 1024, 512, 256);

        std::string message;
        Expect(runtime.submitDirectionKey("a", true, &message), "first direction change should succeed");
        ExpectContains(message, "manual direction set to left", "first direction change should report new direction");

        message.clear();
        Expect(runtime.submitDirectionKey("a", true, &message), "duplicate direction submit should still succeed");
        ExpectContains(message, "manual direction unchanged", "duplicate direction should report no-op");

        const auto telemetry = runtime.telemetry();
        Expect(telemetry.pending_direction == "left", "duplicate direction should keep pending direction");
        Expect(telemetry.current_direction == "right", "duplicate direction should not advance current direction");
        Expect(!telemetry.edge_blocked, "duplicate direction should not set edge_blocked");
    }

    void TestPauseAndEdgeBlockedBoundaries()
    {
        workflow::infer::ManualFlightRuntimeState paused_runtime;
        paused_runtime.configure(workflow::infer::ManualFlightSettings{});
        paused_runtime.activate(1024, 1024, 512, 256);

        cv::Point center;
        Expect(paused_runtime.waitNextCenter([] { return false; }, center), "paused test should get initial center");
        paused_runtime.setPaused(true);
        paused_runtime.markInferenceCommitted(center);

        auto telemetry = paused_runtime.telemetry();
        Expect(telemetry.paused, "paused runtime should expose paused state");
        Expect(telemetry.patch_count == 1, "paused runtime should still count committed patch");

        std::atomic<bool> stop{false};
        std::thread stopper([&stop]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            stop.store(true, std::memory_order_release);
        });
        cv::Point blocked_center;
        const bool got_next = paused_runtime.waitNextCenter([&stop] { return stop.load(std::memory_order_acquire); }, blocked_center);
        stopper.join();
        Expect(!got_next, "paused runtime should not auto-queue next center");

        workflow::infer::ManualFlightRuntimeState edge_runtime;
        edge_runtime.configure(workflow::infer::ManualFlightSettings{});
        edge_runtime.activate(1024, 1024, 512, 256);

        for (int i = 0; i < 3; ++i)
        {
            Expect(edge_runtime.waitNextCenter([] { return false; }, center), "edge test should keep yielding until blocked");
            edge_runtime.markInferenceCommitted(center);
        }

        telemetry = edge_runtime.telemetry();
        Expect(telemetry.patch_count == 3, "edge test should record three commits before blocking");
        Expect(telemetry.edge_blocked, "third commit should block on the right edge");

        std::string message;
        Expect(edge_runtime.submitDirectionKey("a", true, &message), "changing direction away from edge should succeed");
        ExpectContains(message, "manual direction set to left", "edge recovery should report new direction");

        telemetry = edge_runtime.telemetry();
        Expect(!telemetry.edge_blocked, "direction change away from edge should clear edge_blocked");
        Expect(telemetry.current_direction == "left", "direction change away from edge should update current direction");
        Expect(telemetry.pending_direction == "left", "direction change away from edge should update pending direction");

        Expect(edge_runtime.waitNextCenter([] { return false; }, center), "edge recovery should queue next center");
        edge_runtime.markInferenceCommitted(center);
    }
}

int main()
{
    TestSequentialInstancesDoNotLeakState();
    TestInactiveSubmitIsRejected();
    TestDuplicateDirectionSubmitIsNoOp();
    TestPauseAndEdgeBlockedBoundaries();
    std::cout << "manual_flight_runtime_test passed\n";
    return 0;
}
