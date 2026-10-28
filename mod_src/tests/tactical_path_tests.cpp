#include "../tactical_path.h"

#include <cassert>
#include <cstddef>
#include <iostream>

using namespace AutoPlay::TacticalPath;

namespace {

struct TestGrid {
    CellMask passable{};
    EdgeMask edges{};
    CellMask goals{};
    CellMask excluded{};
};

void MakeOpenGrid(TestGrid& grid) {
    grid.passable.fill(1);
    grid.edges.fill(AllDirections);
    grid.goals.fill(0);
    grid.excluded.fill(0);
}

void BlockCell(TestGrid& grid, int x, int z) {
    const int cell = CellIndex(x, z);
    assert(cell >= 0);
    grid.passable[static_cast<size_t>(cell)] = 0;
}

void ConnectLineCell(TestGrid& grid, int x, int z, uint8_t edges) {
    const int cell = CellIndex(x, z);
    assert(cell >= 0);
    grid.passable[static_cast<size_t>(cell)] = 1;
    grid.edges[static_cast<size_t>(cell)] = edges;
}

void Finish(IncrementalPathField& field, size_t budget = 256) {
    size_t iterations = 0;
    while (!field.IsComplete()) {
        assert(field.Step(budget) <= budget);
        assert(++iterations < 1000);
    }
}

void TestObstacleDetourCanInitiallyIncreaseManhattanDistance() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    // A wall blocks the direct route; its only nearby crossing is below it.
    for (int z = 0; z <= 4; ++z) BlockCell(grid, 3, z);
    const int start = CellIndex(1, 2);
    const int goal = CellIndex(5, 2);
    grid.goals[static_cast<size_t>(goal)] = 1;

    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded, start));
    Finish(field);
    const int detour = CellIndex(1, 3);
    const int currentManhattan = 4;
    const int detourManhattan = 5;
    assert(detourManhattan > currentManhattan);
    assert(field.Distance(detour) < field.Distance(start));

    const EndpointCandidate candidates[] = {
        {CellIndex(1, 1), 0, true, false, false, false},
        {detour, 4, true, false, false, false},
        {start, 4, true, false, false, false}
    };
    EndpointChoice choice{};
    assert(ChooseEndpoint(field, candidates, 3, RouteGoalMemory{}, choice));
    assert(choice.cell == detour);
}

void TestUnreachableRoutesAndOutOfBoundsAreRejected() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    for (int z = 0; z < kGridHeight; ++z) BlockCell(grid, 3, z);
    const int start = CellIndex(1, 2);
    grid.goals[static_cast<size_t>(CellIndex(5, 2))] = 1;

    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded, start));
    Finish(field);
    assert(field.Distance(start) == kInfiniteDistance);
    assert(field.Distance(-1, 0) == kInfiniteDistance);
    assert(field.Distance(kGridWidth, 0) == kInfiniteDistance);
    assert(!field.Start(grid.passable, grid.edges, grid.goals, grid.excluded, -1));
    assert(!field.IsActive());

    // The x=0 and x=127 edge cells do not wrap into adjacent columns.
    grid.passable.fill(0);
    grid.edges.fill(AllDirections);
    grid.goals.fill(0);
    grid.passable[static_cast<size_t>(CellIndex(0, 10))] = 1;
    grid.passable[static_cast<size_t>(CellIndex(127, 10))] = 1;
    grid.goals[static_cast<size_t>(CellIndex(0, 10))] = 1;
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded,
                       CellIndex(127, 10)));
    Finish(field);
    assert(field.Distance(127, 10) == kInfiniteDistance);
}

void TestSearchIsIncrementalAndHonorsSmallBudgets() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    const int start = CellIndex(0, 0);
    const int goal = CellIndex(127, 127);
    grid.goals[static_cast<size_t>(goal)] = 1;
    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded, start));
    assert(!field.IsComplete());
    assert(field.Step(0) == 0);
    assert(field.Step(1) == 1);
    assert(!field.IsComplete());
    assert(field.Distance(126, 127) == 1);
    assert(field.Distance(start) == kInfiniteDistance);
    const EndpointCandidate partialCandidate[] = {
        {goal, 0, true, false, false, false}
    };
    EndpointChoice partialChoice{};
    assert(!ChooseEndpoint(field, partialCandidate, 1, RouteGoalMemory{}, partialChoice));
    Finish(field, 256);
    assert(field.Distance(start) == 254);
    assert(field.GoalSource(start) == goal);
}

void TestExcludedAndReservedCellsCannotBeRoutesOrEndpoints() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    const int start = CellIndex(0, 1);
    const int reserved = CellIndex(1, 1);
    const int goal = CellIndex(2, 1);
    grid.goals[static_cast<size_t>(goal)] = 1;
    grid.excluded[static_cast<size_t>(reserved)] = 1;

    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded, start));
    Finish(field);
    assert(field.Distance(reserved) == kInfiniteDistance);
    assert(field.Distance(start) == 4); // Route around the excluded cell.

    const EndpointCandidate candidates[] = {
        {reserved, 0, true, false, false, true},
        {CellIndex(0, 0), 2, true, false, false, false},
        {CellIndex(0, 2), 0, true, true, false, false},
        {CellIndex(1, 0), 0, true, false, true, false},
        {CellIndex(1, 2), 0, true, false, false, true}
    };
    EndpointChoice choice{};
    assert(ChooseEndpoint(field, candidates, 5, RouteGoalMemory{}, choice));
    assert(choice.cell == CellIndex(0, 0));
}

void TestEndpointPrefersSaferTileWhenRouteDistanceTies() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    const int goal = CellIndex(2, 2);
    grid.goals[static_cast<size_t>(goal)] = 1;
    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded,
                       CellIndex(2, 3)));
    Finish(field);

    const EndpointCandidate candidates[] = {
        {CellIndex(2, 1), 8, true, false, false, false},
        {CellIndex(1, 2), 2, true, false, false, false}
    };
    EndpointChoice choice{};
    assert(ChooseEndpoint(field, candidates, 2, RouteGoalMemory{}, choice));
    assert(choice.distance == 1);
    assert(choice.threat == 2);
    assert(choice.cell == CellIndex(1, 2));
}

void TestGoalSourceLockStabilizesEqualRoutes() {
    TestGrid grid{};
    grid.passable.fill(0);
    grid.edges.fill(0);
    grid.goals.fill(0);
    grid.excluded.fill(0);
    for (int x = 0; x <= 4; ++x) {
        ConnectLineCell(grid, x, 0, static_cast<uint8_t>(East | West));
    }
    const int leftGoal = CellIndex(0, 0);
    const int rightGoal = CellIndex(4, 0);
    grid.goals[static_cast<size_t>(leftGoal)] = 1;
    grid.goals[static_cast<size_t>(rightGoal)] = 1;

    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded,
                       CellIndex(2, 0)));
    Finish(field);
    const EndpointCandidate candidates[] = {
        {CellIndex(1, 0), 1, true, false, false, false},
        {CellIndex(3, 0), 1, true, false, false, false}
    };
    RouteGoalMemory memory{};
    memory.goalSource = rightGoal;
    EndpointChoice choice{};
    assert(ChooseEndpoint(field, candidates, 2, memory, choice));
    assert(choice.cell == CellIndex(3, 0));
    assert(memory.goalSource == rightGoal);

    memory.goalSource = -1;
    assert(ChooseEndpoint(field, candidates, 2, memory, choice));
    assert(choice.cell == CellIndex(1, 0)); // Stable coordinate order without a prior lock.
}

void TestEngineIllegalEndpointsAndUnknownThreatAreRejected() {
    TestGrid grid{};
    MakeOpenGrid(grid);
    const int goal = CellIndex(4, 4);
    grid.goals[static_cast<size_t>(goal)] = 1;
    IncrementalPathField field{};
    assert(field.Start(grid.passable, grid.edges, grid.goals, grid.excluded,
                       CellIndex(0, 0)));
    Finish(field);

    const EndpointCandidate candidates[] = {
        {CellIndex(3, 4), 0, false, false, false, false},
        {CellIndex(3, 3), -1, true, false, false, false},
        {CellIndex(3, 2), 0, true, true, false, false},
        {CellIndex(3, 1), 0, true, false, true, false},
        {CellIndex(3, 0), 0, true, false, false, true}
    };
    EndpointChoice choice{};
    assert(!ChooseEndpoint(field, candidates, 5, RouteGoalMemory{}, choice));
    assert(!choice.valid);
}

} // namespace

int main() {
    TestObstacleDetourCanInitiallyIncreaseManhattanDistance();
    TestUnreachableRoutesAndOutOfBoundsAreRejected();
    TestSearchIsIncrementalAndHonorsSmallBudgets();
    TestExcludedAndReservedCellsCannotBeRoutesOrEndpoints();
    TestEndpointPrefersSaferTileWhenRouteDistanceTies();
    TestGoalSourceLockStabilizesEqualRoutes();
    TestEngineIllegalEndpointsAndUnknownThreatAreRejected();
    std::cout << "Tactical path regressions passed\n";
    return 0;
}
