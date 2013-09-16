/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class AuthorizationManager;

namespace auth {

    /**
     * Writes into *writeConcern a BSONObj describing the parameters to getLastError to use for
     * the write confirmation.
     */
    Status extractWriteConcern(const BSONObj& cmdObj, BSONObj* writeConcern);

    /**
     * Takes a command object describing an invocation of the "createUser" command on the database
     * "dbname", and returns (via the output param "parsedUserObj") a user object that can be
     * inserted into admin.system.users to create the user as described by the command object.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateCreateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUserObj);

    /**
     * Takes a command object describing an invocation of the "updateUser" command on the database
     * "dbname", and returns (via the output param "parsedUpdateObj") an update specifier that can
     * be used to update the user document in admin.system.users as described by the command object,
     * as well as the user name of the user being updated (via the "parsedUserName" output param).
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateUpdateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUpdateObj,
                                             UserName* parsedUserName);

    /**
     * Takes a command object describing an invocation of one of "grantRolesToUser",
     * "revokeRolesFromUser", "grantDelegateRolesToUser", and "revokeDelegateRolesFromUser" (which
     * command it is is specified in the "cmdName" argument), and parses out the user name of the
     * user being modified, the roles being granted or revoked, and the write concern to use.
     */
    Status parseUserRoleManipulationCommand(const BSONObj& cmdObj,
                                            const StringData& cmdName,
                                            const std::string& dbname,
                                            AuthorizationManager* authzManager,
                                            UserName* parsedUserName,
                                            vector<RoleName>* parsedRoleNames,
                                            BSONObj* parsedWriteConcern);

} // namespace auth
} // namespace mongo
