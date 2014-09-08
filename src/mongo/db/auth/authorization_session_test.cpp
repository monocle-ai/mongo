/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * Unit tests of the AuthorizationSession type.
 */

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespacestring.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {


    class AuthorizationSessionTest : public ::mongo::unittest::Test {
    public:
        AuthzManagerExternalStateMock* managerState;
        AuthzSessionExternalStateMock* sessionState;
        scoped_ptr<AuthorizationSession> authzSession;
        scoped_ptr<AuthorizationManager> authzManager;

        void setUp() {
            managerState = new AuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(managerState));
            sessionState = new AuthzSessionExternalStateMock(authzManager.get());
            authzSession.reset(new AuthorizationSession(sessionState));
        }
    };

    TEST_F(AuthorizationSessionTest, AcquirePrivilegeAndCheckAuthorization) {
        Principal* principal = new Principal(UserName("Spencer", "test"));
        ActionSet actions;
        actions.addAction(ActionType::insert);
        Privilege writePrivilege("test", actions);
        Privilege allDBsWritePrivilege("*", actions);

        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));
        sessionState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));
        sessionState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authzSession->acquirePrivilege(writePrivilege, principal->getName()));
        authzSession->addAndAuthorizePrincipal(principal);
        ASSERT_OK(authzSession->acquirePrivilege(writePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession->checkAuthorization("test", ActionType::insert));

        ASSERT_FALSE(authzSession->checkAuthorization("otherDb", ActionType::insert));
        ASSERT_OK(authzSession->acquirePrivilege(allDBsWritePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession->checkAuthorization("otherDb", ActionType::insert));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authzSession->checkAuthorization("otherDb.collectionName", ActionType::insert));

        authzSession->logoutDatabase("test");
        ASSERT_FALSE(authzSession->checkAuthorization("test", ActionType::insert));
    }


    TEST_F(AuthorizationSessionTest, ImplicitAcquireFromSomeDatabases) {
        managerState->insertPrivilegeDocument("test",
                                    BSON("user" << "andy" <<
                                         "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("readWrite")));
        managerState->insertPrivilegeDocument("test2",
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" <<  BSON_ARRAY("read")));
        managerState->insertPrivilegeDocument("admin",
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" << BSON_ARRAY("clusterAdmin") <<
                                         "otherDBRoles" << BSON("test3" << BSON_ARRAY("dbAdmin"))));

        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("$SERVER", ActionType::shutdown));

        Principal* principal = new Principal(UserName("andy", "test"));
        authzSession->addAndAuthorizePrincipal(principal);

        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("$SERVER", ActionType::shutdown));
    }

}  // namespace
}  // namespace mongo
