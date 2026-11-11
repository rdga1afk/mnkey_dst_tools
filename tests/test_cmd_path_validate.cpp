#include <gtest/gtest.h>
#include <monkey_dust/editor/cmd_path_validate.h>

// EDITOR_AUTOMATION_PLAN_v1.md Phase 2.4: path allowlist for every command/
// Lua binding taking a filesystem path. Purely lexical (no realpath/stat),
// so it must be tested against exactly the traversal shapes a real
// adversarial input could take, not just "does the obvious case work".

TEST(CmdPathValidate, AcceptsDataRoot) {
    EXPECT_TRUE(CmdPathValidate("data/scenes/foo.json"));
}

TEST(CmdPathValidate, AcceptsGameDataRoot) {
    EXPECT_TRUE(CmdPathValidate("game/data/saves/scene.json"));
}

TEST(CmdPathValidate, AcceptsAutomationOutRoot) {
    EXPECT_TRUE(CmdPathValidate("automation_out/capture_001.png"));
}

TEST(CmdPathValidate, AcceptsScriptsRoot) {
    // Trusted first-party authoring-library source, read via
    // md.editor.load_script() — the sandboxed Lua dofile/loadfile
    // alternative (Phase 3).
    EXPECT_TRUE(CmdPathValidate("scripts/authoring/authoring.lua"));
}

TEST(CmdPathValidate, RejectsNullAndEmpty) {
    EXPECT_FALSE(CmdPathValidate(nullptr));
    EXPECT_FALSE(CmdPathValidate(""));
}

TEST(CmdPathValidate, RejectsAbsolutePath) {
    EXPECT_FALSE(CmdPathValidate("/etc/passwd"));
    EXPECT_FALSE(CmdPathValidate("/data/scenes/foo.json")); // absolute even with an allowed-looking tail
}

TEST(CmdPathValidate, RejectsHomeTilde) {
    EXPECT_FALSE(CmdPathValidate("~/data/foo.json"));
}

TEST(CmdPathValidate, RejectsOutsideAllowlist) {
    EXPECT_FALSE(CmdPathValidate("tmp_/whatever.txt"));
    EXPECT_FALSE(CmdPathValidate("engine/include/foo.h"));
    EXPECT_FALSE(CmdPathValidate("terrain_research/whatever.md"));
}

// The plan's own explicit negative test: "exec Save As ../../etc/x must fail".
TEST(CmdPathValidate, RejectsLeadingTraversal) {
    EXPECT_FALSE(CmdPathValidate("../../etc/x"));
}

TEST(CmdPathValidate, RejectsTraversalAfterAllowedPrefix) {
    // Starts with an allowed root as a literal string prefix, but the ".."
    // segment backs all the way out past it — must still be rejected.
    EXPECT_FALSE(CmdPathValidate("data/../../../etc/passwd"));
    EXPECT_FALSE(CmdPathValidate("game/data/../../../../etc/passwd"));
}

TEST(CmdPathValidate, RejectsTraversalInMiddleOfPath) {
    EXPECT_FALSE(CmdPathValidate("data/foo/../../bar"));
}

TEST(CmdPathValidate, RejectsDotDotAsWholeSegmentOnlyNotSubstring) {
    // A filename that merely CONTAINS ".." but isn't a ".." segment on its
    // own must not be falsely rejected — the check is segment-bounded.
    EXPECT_TRUE(CmdPathValidate("data/scenes/my..weird..file.json"));
}

TEST(CmdPathValidate, RejectsTrailingTraversalSegment) {
    // ".." as the FINAL segment (no trailing slash) must also be caught —
    // exercises the p[2] == '\0' branch, not just p[2] == '/'.
    EXPECT_FALSE(CmdPathValidate("data/.."));
}
