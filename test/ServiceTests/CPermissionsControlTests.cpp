#include "service/CPermissionsControl.h"

#include <gtest/gtest.h>
#include <sptk5/sptk.h>

using namespace xmq;
using namespace sptk;

/**
 * The optional choices are plain strings, not enumerations: a generated enumeration is checked
 * even when its element is absent, so "list" would be refused for not carrying an import format.
 * What these tests pin down is that a request that does not use an optional field may leave it out.
 */
TEST(CPermissionsControlTest, listNeedsNoImportOrTestFields)
{
    CPermissionsControl control;
    control.m_action = "list";
    EXPECT_NO_THROW(control.checkRestrictions());
}

TEST(CPermissionsControlTest, importNeedsNoTestAction)
{
    CPermissionsControl control;
    control.m_action = "import-plan";
    control.m_format = "acl";
    control.m_content = "topic read #\n";
    EXPECT_NO_THROW(control.checkRestrictions());
}

TEST(CPermissionsControlTest, testNeedsNoImportFields)
{
    CPermissionsControl control;
    control.m_action = "test";
    control.m_username = "alice";
    control.m_topic = "sensors/#";
    EXPECT_NO_THROW(control.checkRestrictions());
}

TEST(CPermissionsControlTest, anActionIsStillRequired)
{
    CPermissionsControl control;
    EXPECT_THROW(control.checkRestrictions(), Exception);
}
