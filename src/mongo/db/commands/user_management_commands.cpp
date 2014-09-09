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

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/user_management_commands_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    static void addStatus(const Status& status, BSONObjBuilder& builder) {
        builder.append("ok", status.isOK() ? 1.0: 0.0);
        if (!status.isOK())
            builder.append("code", status.code());
        if (!status.reason().empty())
            builder.append("errmsg", status.reason());
    }

    static void redactPasswordData(mutablebson::Element parent) {
        namespace mmb = mutablebson;
        const StringData pwdFieldName("pwd", StringData::LiteralTag());
        for (mmb::Element pwdElement = mmb::findFirstChildNamed(parent, pwdFieldName);
             pwdElement.ok();
             pwdElement = mmb::findElementNamed(pwdElement.rightSibling(), pwdFieldName)) {

            pwdElement.setValueString("xxx");
        }
    }

    static BSONArray rolesToBSONArray(const User::RoleDataMap& roles) {
        BSONArrayBuilder arrBuilder;
        for (User::RoleDataMap::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            const User::RoleData& role = it->second;
            arrBuilder.append(BSON("name" << role.name.getRole() <<
                                   "source" << role.name.getDB() <<
                                   "hasRole" << role.hasRole <<
                                   "canDelegate" << role.canDelegate));
        }
        return arrBuilder.arr();
    }

    static Status getCurrentUserRoles(AuthorizationManager* authzManager,
                                      const UserName& userName,
                                      User::RoleDataMap* roles) {
        User* user;
        Status status = authzManager->acquireUser(userName, &user);
        if (!status.isOK()) {
            return status;
        }
        *roles = user->getRoles();
        authzManager->releaseUser(user);
        return Status::OK();
    }

    class CmdCreateUser : public InformationCommand {
    public:
        // This is an InformationCommand because all the txn management stuff happens inside
        // AuthzManagerExternalStateMongod.

        CmdCreateUser() : InformationCommand("createUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Adds a user to the system" << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Create user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            BSONObj userObj;
            Status status = auth::parseAndValidateCreateUserCommand(cmdObj,
                                                                    dbname,
                                                                    authzManager,
                                                                    &userObj);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONObj writeConcern;
            status = auth::extractWriteConcern(cmdObj, &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = authzManager->insertPrivilegeDocument(dbname, userObj, writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }
            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    } cmdCreateUser;

    class CmdUpdateUser : public InformationCommand {
    public:

        CmdUpdateUser() : InformationCommand("updateUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Used to update a user, for example to change its password" << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Update user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            BSONObj updateObj;
            UserName userName;
            Status status = auth::parseAndValidateUpdateUserCommand(cmdObj,
                                                                    dbname,
                                                                    authzManager,
                                                                    &updateObj,
                                                                    &userName);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONObj writeConcern;
            status = auth::extractWriteConcern(cmdObj, &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            status = authzManager->updatePrivilegeDocument(userName, updateObj, writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

        virtual void redactForLogging(mutablebson::Document* cmdObj) {
            redactPasswordData(cmdObj->root());
        }

    } cmdUpdateUser;

    class CmdRemoveUser : public InformationCommand {
    public:

        CmdRemoveUser() : InformationCommand("removeUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Removes a single user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Remove user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            std::string user;
            Status status = bsonExtractStringField(cmdObj, "removeUser", &user);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            BSONObj writeConcern;
            status = auth::extractWriteConcern(cmdObj, &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            int numUpdated;
            status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_NAME_FIELD_NAME << user <<
                         AuthorizationManager::USER_SOURCE_FIELD_NAME << dbname),
                    writeConcern,
                    &numUpdated);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            if (numUpdated == 0) {
                addStatus(Status(ErrorCodes::UserNotFound,
                                 mongoutils::str::stream() << "User '" << user << "@" <<
                                         dbname << "' not found"),
                          result);
                return false;
            }

            authzManager->invalidateUserByName(UserName(user, dbname));
            return true;
        }

    } cmdRemoveUser;

    class CmdRemoveUsersFromDatabase : public InformationCommand {
    public:

        CmdRemoveUsersFromDatabase() : InformationCommand("removeUsersFromDatabase") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Removes all users for a single database." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Remove all users from database")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            BSONObj writeConcern;
            Status status = auth::extractWriteConcern(cmdObj, &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            int numRemoved;
            status = authzManager->removePrivilegeDocuments(
                    BSON(AuthorizationManager::USER_SOURCE_FIELD_NAME << dbname),
                    writeConcern,
                    &numRemoved);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            result.append("n", numRemoved);

            authzManager->invalidateUsersFromDB(dbname);
            return true;
        }

    } cmdRemoveUsersFromDatabase;

    class CmdGrantRolesToUser: public InformationCommand {
    public:

        CmdGrantRolesToUser() : InformationCommand("grantRolesToUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Grants roles to a user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Grant roles to user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            UserName userName;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            Status status = auth::parseUserRoleManipulationCommand(cmdObj,
                                                                   "grantRolesToUser",
                                                                   dbname,
                                                                   authzManager,
                                                                   &userName,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            User::RoleDataMap userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                User::RoleData& role = userRoles[roleName];
                if (role.name.empty()) {
                    role.name = roleName;
                }
                role.hasRole = true;
            }

            BSONArray newRolesBSONArray = rolesToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

    } cmdGrantRolesToUser;

    class CmdRevokeRolesFromUser: public InformationCommand {
    public:

        CmdRevokeRolesFromUser() : InformationCommand("revokeRolesFromUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Revokes roles from a user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Revoke roles from user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            UserName userName;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            Status status = auth::parseUserRoleManipulationCommand(cmdObj,
                                                                   "revokeRolesFromUser",
                                                                   dbname,
                                                                   authzManager,
                                                                   &userName,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            User::RoleDataMap userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                User::RoleDataMap::iterator roleDataIt = userRoles.find(roleName);
                if (roleDataIt == userRoles.end()) {
                    continue; // User already doesn't have the role, nothing to do
                }
                User::RoleData& role = roleDataIt->second;
                if (role.canDelegate) {
                    // If the user can still delegate the role, need to leave it in the roles array
                    role.hasRole = false;
                } else {
                    // If the user can't delegate the role, and now doesn't have it either, remove
                    // the role from that user's roles array entirely
                    userRoles.erase(roleDataIt);
                }
            }

            BSONArray newRolesBSONArray = rolesToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

    } cmdRevokeRolesFromUser;

    class CmdGrantDelegateRolesToUser: public InformationCommand {
    public:

        CmdGrantDelegateRolesToUser() : InformationCommand("grantDelegateRolesToUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Grants the right to delegate roles to a user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Grant role delegation to user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            UserName userName;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            Status status = auth::parseUserRoleManipulationCommand(cmdObj,
                                                                   "grantDelegateRolesToUser",
                                                                   dbname,
                                                                   authzManager,
                                                                   &userName,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            User::RoleDataMap userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                User::RoleData& role = userRoles[roleName];
                if (role.name.empty()) {
                    role.name = roleName;
                }
                role.canDelegate = true;
            }

            BSONArray newRolesBSONArray = rolesToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

    } cmdGrantDelegateRolesToUser;

    class CmdRevokeDelegateRolesFromUser: public InformationCommand {
    public:

        CmdRevokeDelegateRolesFromUser() : InformationCommand("revokeDelegateRolesFromUser") {}

        virtual bool slaveOk() const {
            return false;
        }

        virtual void help(stringstream& ss) const {
            ss << "Revokes the right to delegate roles from a user." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("Revoke role delegation from user")) {
                addStatus(Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."),
                          result);
                return false;
            }

            UserName userName;
            std::vector<RoleName> roles;
            BSONObj writeConcern;
            Status status = auth::parseUserRoleManipulationCommand(cmdObj,
                                                                   "revokeDelegateRolesFromUser",
                                                                   dbname,
                                                                   authzManager,
                                                                   &userName,
                                                                   &roles,
                                                                   &writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            User::RoleDataMap userRoles;
            status = getCurrentUserRoles(authzManager, userName, &userRoles);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            for (vector<RoleName>::iterator it = roles.begin(); it != roles.end(); ++it) {
                RoleName& roleName = *it;
                User::RoleDataMap::iterator roleDataIt = userRoles.find(roleName);
                if (roleDataIt == userRoles.end()) {
                    continue; // User already doesn't have the role, nothing to do
                }
                User::RoleData& role = roleDataIt->second;
                if (role.hasRole) {
                    // If the user still has the role, need to leave it in the roles array
                    role.canDelegate = false;
                } else {
                    // If the user doesn't have the role, and now can't delegate it either, remove
                    // the role from that user's roles array entirely
                    userRoles.erase(roleDataIt);
                }
            }

            BSONArray newRolesBSONArray = rolesToBSONArray(userRoles);
            status = authzManager->updatePrivilegeDocument(
                    userName, BSON("$set" << BSON("roles" << newRolesBSONArray)), writeConcern);
            if (!status.isOK()) {
                addStatus(status, result);
                return false;
            }

            authzManager->invalidateUserByName(userName);
            return true;
        }

    } cmdRevokeDelegateRolesFromUser;

    class CmdUsersInfo: public InformationCommand {
    public:

        virtual bool slaveOk() const {
            return false;
        }

        CmdUsersInfo() : InformationCommand("usersInfo") {}

        virtual void help(stringstream& ss) const {
            ss << "Returns information about users." << endl;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: update this with the new rules around user creation in 2.6.
            ActionSet actions;
            actions.addAction(ActionType::userAdmin);
            out->push_back(Privilege(dbname, actions));
        }

        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            if (cmdObj["usersInfo"].type() != String && cmdObj["usersInfo"].type() != RegEx) {
                addStatus(Status(ErrorCodes::BadValue,
                                 "Argument to userInfo command must be either a string or a regex"),
                          result);
                return false;
            }

            bool anyDB = false;
            if (cmdObj.hasField("anyDB")) {
                if (dbname == "admin") {
                    Status status = bsonExtractBooleanField(cmdObj, "anyDB", &anyDB);
                    if (!status.isOK()) {
                        addStatus(status, result);
                        return false;
                    }
                } else {
                    addStatus(Status(ErrorCodes::BadValue,
                                     "\"anyDB\" argument to usersInfo command is only valid when "
                                     "run on the \"admin\" database"),
                              result);
                    return false;
                }
            }

            BSONObjBuilder queryBuilder;
            queryBuilder.appendAs(cmdObj["usersInfo"], "name");
            if (!anyDB) {
                queryBuilder.append("source", dbname);
            }

            BSONArrayBuilder usersArrayBuilder;
            BSONArrayBuilder& (BSONArrayBuilder::* appendBSONObj) (const BSONObj&) =
                    &BSONArrayBuilder::append<BSONObj>;
            const boost::function<void(const BSONObj&)> function =
                    boost::bind(appendBSONObj, &usersArrayBuilder, _1);
            AuthorizationManager* authzManager = getGlobalAuthorizationManager();
            authzManager->queryAuthzDocument(NamespaceString("admin.system.users"),
                                             queryBuilder.done(),
                                             function);

            result.append("users", usersArrayBuilder.arr());
            return true;
        }

    } cmdUsersInfo;
}
