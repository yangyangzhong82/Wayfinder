#include "wayfinder/MapStorage.h"
#include "wayfinder/Navigation.h"
#include "wayfinder/WorldIdentity.h"
#include <chrono>
#include <iostream>

using namespace wayfinder;

namespace {
void require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}
struct TestDirectory {
    std::filesystem::path path =
        std::filesystem::path("build/tests")
        / ("navigation-persistence-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { require(std::filesystem::create_directories(path), "Create unique test directory"); }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path navigationPath(std::string const& identity) const {
        auto result = path / storageName(identity);
        result.replace_extension(".json");
        return result;
    }
};
} // namespace

int main() {
    // Reproduce the reported world: multiplayer=true, but LevelId is available.
    // Multiplayer is deliberately absent from the identity selection API.
    auto first = worldStorageIdentity("", "cdxhG2iMotU=", "17905837949027565");
    auto next  = worldStorageIdentity("", "cdxhG2iMotU=", "second-launch");
    require(first == "local:cdxhG2iMotU=" && first == next, "Reported world keeps its identity across restarts");
    require(
        worldStorageIdentity("", "", "first-tick") == "session:first-tick",
        "Missing LevelId must not block map startup"
    );
    require(
        worldStorageIdentity("", "", "another-launch") == "session:another-launch",
        "Missing world identities remain isolated across launches"
    );
    require(
        worldStorageIdentity("my-world", "cdxhG2iMotU=", "first-tick") == "profile:my-world",
        "Explicit profiles remain compatible"
    );
    require(
        worldStorageIdentity("", "remote-world-id", "remote-launch") == "local:remote-world-id",
        "A stable LevelId remains deterministic across multiplayer state"
    );
    auto other = worldStorageIdentity("", "local-world-b", "first-launch");
    require(other != first, "Different local worlds stay isolated");
    for (auto profile : {"", "my-world"}) {
        for (auto levelId : {"", "cdxhG2iMotU="}) {
            auto identity = worldStorageIdentity(profile, levelId, "startup");
            require(!identity.empty(), "Every startup state must yield a usable identity");
        }
    }

    TestDirectory directory;
    auto          firstPath   = directory.navigationPath(first);
    auto          restartPath = directory.navigationPath(next);
    require(firstPath == restartPath, "Restart opens the same JSON filename");
    Waypoint point;
    point.name    = "测试路标";
    point.x       = -321;
    point.y       = 72;
    point.z       = 456;
    point.color   = 5;
    point.icon    = 1;
    point.created = 1790553600;
    std::vector<Waypoint> savedPoints;
    std::uint64_t         target{};
    {
        Navigation original;
        target          = original.save(point);
        original.target = target;
        point.name      = "Portal";
        point.y.reset();
        point.dimension = 1;
        point.icon      = 2;
        original.save(point);
        savedPoints = original.points;
        writeNavigation(firstPath, first, original);
    } // The previous session state is gone; reload solely from disk.
    auto restored = readNavigation(restartPath, next);
    require(restored.points == savedPoints && restored.target == target, "Waypoints and navigation survive restart");
    require(readNavigation(directory.navigationPath(other), other).points.empty(), "No cross-world waypoint leakage");
    require(!std::filesystem::exists(firstPath.string() + ".tmp"), "Atomic save completed");

    bool mismatchRejected = false;
    try {
        readNavigation(firstPath, other);
    } catch (std::runtime_error const&) {
        mismatchRejected = true;
    }
    require(mismatchRejected, "Wrong-world data is rejected");
    auto edited = *restored.find(target);
    edited.name = "Edited after restart";
    edited.x    = -999;
    restored.save(edited);
    writeNavigation(restartPath, next, restored);
    auto editedReload = readNavigation(restartPath, next);
    require(editedReload.find(target) && *editedReload.find(target) == edited, "Edits survive another restart");
    editedReload.remove(target);
    writeNavigation(restartPath, next, editedReload);
    auto deletedReload = readNavigation(restartPath, next);
    require(
        !deletedReload.find(target) && deletedReload.target == 0 && deletedReload.points.size() == 1,
        "Deletion and stopped navigation survive restart"
    );
    std::cout << "Non-blocking startup, isolated worlds/profiles and waypoint save/edit/delete passed.\n";
}
