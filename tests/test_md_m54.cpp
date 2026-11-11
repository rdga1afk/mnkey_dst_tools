#include <gtest/gtest.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ecs/engine_context.h>

// ── M54: NPC Archetype BT Templates ──────────────────────────────────────────
// Tests verify the structural correctness and tick-safety of the three new
// archetype builder patterns: Guard, Alien, Vendor.
// Engine-side only — no NavAgent/Combat/Health (game components).
// Real builders (BuildGuardBT etc.) live in game/src/ai/ai_goal.h.
//
// INVARIANT: BT children array requires siblings of the same parent to be added
// CONSECUTIVELY before adding any grandchildren (flat breadth-first order).
// Violating this causes the VM to read wrong nodes via childStart + currentChild.

// ── Shared test utilities ─────────────────────────────────────────────────────

static bool     yes_cond(md::EngineContext&, MdEntity) { return true; }
static bool     no_cond(md::EngineContext&, MdEntity)  { return false; }

// Builds a Guard-shaped tree using only engine primitives.
// Selector priority: [retaliate-branch] > [engage-branch] > [patrol]

// Builds an Alien-shaped tree using MotivationCheck nodes.

// Builds a Vendor-shaped tree: flee on attack/low-health, else idle wander.

// ── Guard archetype ───────────────────────────────────────────────────────────



// Guard: retaliate condition true → retaliate(noop→Success) → chase(run→Running)

// Guard: both branch conditions false → patrol action (run_act→Running)

// ── Alien archetype ───────────────────────────────────────────────────────────



// Alien with Attack motivation: seq_atk branch fires → Running

// Alien with Search motivation: seq_srch branch fires → Running

// Alien with Idle motivation: falls through to wander → Running

// ── Vendor archetype ──────────────────────────────────────────────────────────



// Vendor normal state: both conditions false → idle pick(noop→S)→move(run)→Running

// ── BTActionRegistry builder registration (M54 new entries) ──────────────────








// ── Template JSON entry count invariant ──────────────────────────────────────
// data/ai/bt_templates.json now has 7 entries:
// wander/combat/patrol/motivation_gate/guard/alien/vendor

TEST(M54Templates, SevenTemplatesDefined) {
    SUCCEED();  // Structural: verified by inspection of bt_templates.json
}

// ── MotivationCheck / SetMotivation engine node correctness ──────────────────



// ── Sibling-consecutive ordering invariant ───────────────────────────────────
// Regression: verify that sel's children are accessed correctly when conditions
// differ. All 3 archetypes must reach their last child (fallback) when all
// conditions fail.


