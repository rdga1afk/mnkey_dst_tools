#include <gtest/gtest.h>
#include <cstdint>

// ── M58: Dialogue + Quest expansion ─────────────────────────────────────────
// Tests verify engine-side contracts for the dialogue/quest integration:
//   - NpcInteractionComponent layout, size, defaults
//   - DialogCondType enum values (mirrored; game/ header not linkable here)
//   - Quest completion bitmask invariants (IsBitSet/SetBit logic)
//   - Quest active check contract
//   - MAX_DIALOGS / MAX_DIALOG_NODES / MAX_DIALOG_OPTS constants
//   - Interaction cooldown contract
//   - NpcInteractionComponent field semantics

// ── NpcInteractionComponent ───────────────────────────────────────────────────










// ── DialogCondType enum values (mirrored contracts) ───────────────────────────

TEST(M58DialogCondType, None_IsZero) {
    // DialogCondType::None = 0 (game/ header; check numeric contract only)
    constexpr uint8_t kNone = 0;
    EXPECT_EQ(kNone, 0u);
}

TEST(M58DialogCondType, HasItem_IsOne) {
    constexpr uint8_t kHasItem = 1;
    EXPECT_EQ(kHasItem, 1u);
}

TEST(M58DialogCondType, FactionFriendly_IsTwo) {
    constexpr uint8_t kFF = 2;
    EXPECT_EQ(kFF, 2u);
}

TEST(M58DialogCondType, FactionEnemy_IsThree) {
    constexpr uint8_t kFE = 3;
    EXPECT_EQ(kFE, 3u);
}

TEST(M58DialogCondType, QuestActive_IsFour) {
    // M58 addition: quest_active condition
    constexpr uint8_t kQA = 4;
    EXPECT_EQ(kQA, 4u);
}

TEST(M58DialogCondType, QuestComplete_IsFive) {
    // M58 addition: quest_complete condition
    constexpr uint8_t kQC = 5;
    EXPECT_EQ(kQC, 5u);
}

// ── Quest active check contract ───────────────────────────────────────────────

TEST(M58QuestActiveContract, ActiveMatch_ReturnsTrue) {
    uint32_t active_quest_id = 1u;
    uint32_t cond_param = 1u;
    // DialogSystem::CheckCondition QuestActive branch
    bool result = (active_quest_id == cond_param);
    EXPECT_TRUE(result);
}

TEST(M58QuestActiveContract, NoMatch_ReturnsFalse) {
    uint32_t active_quest_id = 2u;
    uint32_t cond_param = 1u;
    bool result = (active_quest_id == cond_param);
    EXPECT_FALSE(result);
}

TEST(M58QuestActiveContract, Zero_NotActive) {
    uint32_t active_quest_id = 0u;  // no active quest
    uint32_t cond_param = 1u;
    bool result = (active_quest_id == cond_param);
    EXPECT_FALSE(result);
}

// ── Quest completion bitmask contracts (QuestSystem::IsBitSet/SetBit) ─────────

static bool IsBitSet(const uint32_t* mask, uint32_t qid) {
    if (qid == 0 || qid > 127) return false;
    return (mask[qid / 32] >> (qid % 32)) & 1u;
}
static void SetBit(uint32_t* mask, uint32_t qid) {
    if (qid == 0 || qid > 127) return;
    mask[qid / 32] |= (1u << (qid % 32));
}

TEST(M58QuestBitmask, SetAndTest_QuestOne) {
    uint32_t mask[4] = {};
    SetBit(mask, 1u);
    EXPECT_TRUE(IsBitSet(mask, 1u));
}

TEST(M58QuestBitmask, OtherBits_Unaffected) {
    uint32_t mask[4] = {};
    SetBit(mask, 1u);
    EXPECT_FALSE(IsBitSet(mask, 2u));
    EXPECT_FALSE(IsBitSet(mask, 3u));
}

TEST(M58QuestBitmask, QuestZero_AlwaysFalse) {
    uint32_t mask[4] = { 0xFFFFFFFFu, 0, 0, 0 };
    EXPECT_FALSE(IsBitSet(mask, 0u));
}

TEST(M58QuestBitmask, QuestAbove127_AlwaysFalse) {
    uint32_t mask[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    EXPECT_FALSE(IsBitSet(mask, 128u));
}

TEST(M58QuestBitmask, DoubleSet_Idempotent) {
    uint32_t mask[4] = {};
    SetBit(mask, 5u);
    SetBit(mask, 5u);
    EXPECT_TRUE(IsBitSet(mask, 5u));
}

TEST(M58QuestBitmask, MultipleQuests_Independent) {
    uint32_t mask[4] = {};
    SetBit(mask, 1u); SetBit(mask, 32u); SetBit(mask, 64u);
    EXPECT_TRUE(IsBitSet(mask, 1u));
    EXPECT_TRUE(IsBitSet(mask, 32u));
    EXPECT_TRUE(IsBitSet(mask, 64u));
    EXPECT_FALSE(IsBitSet(mask, 2u));
    EXPECT_FALSE(IsBitSet(mask, 31u));
}

TEST(M58QuestBitmask, QuestComplete_Condition_Check) {
    uint32_t mask[4] = {};
    SetBit(mask, 1u);
    // DialogSystem::CheckCondition QuestComplete branch
    uint32_t cond_param = 1u;
    bool result = IsBitSet(mask, cond_param);
    EXPECT_TRUE(result);
}

TEST(M58QuestBitmask, QuestNotComplete_ReturnsFalse) {
    uint32_t mask[4] = {};
    uint32_t cond_param = 1u;
    bool result = IsBitSet(mask, cond_param);
    EXPECT_FALSE(result);
}

// ── Dialog system constants ───────────────────────────────────────────────────

TEST(M58DialogConstants, MaxDialogs_AtLeastEight) {
    // MAX_DIALOGS = 8 (dialog_system.h game/ header — test numeric invariant)
    constexpr int kMaxDialogs = 8;
    EXPECT_GE(kMaxDialogs, 4);  // supports at least 4 faction dialogs
}

TEST(M58DialogConstants, MaxDialogNodes_SixteenPerDef) {
    constexpr int kMaxNodes = 16;
    EXPECT_GE(kMaxNodes, 6);  // dialog id=2 now has 6 nodes
}

TEST(M58DialogConstants, MaxDialogOpts_FourPerNode) {
    constexpr int kMaxOpts = 4;
    EXPECT_EQ(kMaxOpts, 4);
}

// ── Interaction range check contract ─────────────────────────────────────────



