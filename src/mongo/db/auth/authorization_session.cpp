/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/auth/authorization_session.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespacestring.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    const std::string ADMIN_DBNAME = "admin";
}  // namespace

    AuthorizationSession::AuthorizationSession(AuthzSessionExternalState* externalState) {
        _externalState.reset(externalState);
    }

    AuthorizationSession::~AuthorizationSession() {
        for (UserSet::iterator it = _authenticatedUsers.begin();
                it != _authenticatedUsers.end(); ++it) {
            getAuthorizationManager().releaseUser(*it);
        }
    }

    AuthorizationManager& AuthorizationSession::getAuthorizationManager() {
        return _externalState->getAuthorizationManager();
    }

    void AuthorizationSession::startRequest() {
        _externalState->startRequest();
    }

    Status AuthorizationSession::addAndAuthorizeUser(const UserName& userName) {
        User* user;
        Status status = getAuthorizationManager().acquireUser(userName, &user);
        if (!status.isOK()) {
            return status;
        }

        // Calling add() on the UserSet may return a user that was replaced because it was from the
        // same database.
        User* replacedUser = _authenticatedUsers.add(user);
        if (replacedUser) {
            getAuthorizationManager().releaseUser(replacedUser);
        }

        return Status::OK();
    }

    User* AuthorizationSession::lookupUser(const UserName& name) {
        return _authenticatedUsers.lookup(name);
    }

    void AuthorizationSession::logoutDatabase(const std::string& dbname) {
        User* removedUser = _authenticatedUsers.removeByDBName(dbname);
        if (removedUser) {
            getAuthorizationManager().releaseUser(removedUser);
        }
    }

    UserSet::NameIterator AuthorizationSession::getAuthenticatedUserNames() {
        return _authenticatedUsers.getNames();
    }

    std::string AuthorizationSession::getAuthenticatedUserNamesToken() {
        std::string ret;
        for (UserSet::NameIterator nameIter = getAuthenticatedUserNames();
                nameIter.more();
                nameIter.next()) {
            ret += '\0'; // Using a NUL byte which isn't valid in usernames to separate them.
            ret += nameIter->getFullName();
        }

        return ret;
    }

    void AuthorizationSession::grantInternalAuthorization() {
        _authenticatedUsers.add(internalSecurity.user);
    }

    Status AuthorizationSession::checkAuthForQuery(const NamespaceString& ns,
                                                   const BSONObj& query) {
        if (MONGO_unlikely(ns.isCommand())) {
            return Status(ErrorCodes::InternalError, mongoutils::str::stream() <<
                          "Checking query auth on command namespace " << ns.ns());
        }
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for query on " << ns.ns());
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForGetMore(const NamespaceString& ns,
                                                     long long cursorID) {
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized for getmore on " << ns.ns());
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForInsert(const NamespaceString& ns,
                                                    const BSONObj& document,
                                                    bool buildingSystemUsersIndex) {
        if (ns.coll == StringData("system.indexes", StringData::LiteralTag())) {
            BSONElement nsElement = document["ns"];
            if (nsElement.type() != String) {
                return Status(ErrorCodes::Unauthorized, "Cannot authorize inserting into "
                              "system.indexes documents without a string-typed \"ns\" field.");
            }
            NamespaceString indexNS(nsElement.str());
            if (!buildingSystemUsersIndex &&
                !isAuthorizedForActionsOnNamespace(indexNS, ActionType::ensureIndex)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized to create index on " <<
                              indexNS.ns());
            }
        } else {
            if (!isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for insert on " <<
                              ns.ns());
            }
        }

        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForUpdate(const NamespaceString& ns,
                                                    const BSONObj& query,
                                                    const BSONObj& update,
                                                    bool upsert) {
        if (!upsert) {
            if (!isAuthorizedForActionsOnNamespace(ns, ActionType::update)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for update on " <<
                              ns.ns());
            }
        }
        else {
            ActionSet required;
            required.addAction(ActionType::update);
            required.addAction(ActionType::insert);
            if (!isAuthorizedForActionsOnNamespace(ns, required)) {
                return Status(ErrorCodes::Unauthorized,
                              mongoutils::str::stream() << "not authorized for upsert on " <<
                              ns.ns());
            }
        }
        return Status::OK();
    }

    Status AuthorizationSession::checkAuthForDelete(const NamespaceString& ns,
                                                    const BSONObj& query) {
        if (!isAuthorizedForActionsOnNamespace(ns, ActionType::remove)) {
            return Status(ErrorCodes::Unauthorized,
                          mongoutils::str::stream() << "not authorized to remove from " << ns.ns());
        }
        return Status::OK();
    }


    bool AuthorizationSession::isAuthorizedForPrivilege(const Privilege& privilege) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        return _isAuthorizedForPrivilege(privilege);
    }

    bool AuthorizationSession::isAuthorizedForPrivileges(const vector<Privilege>& privileges) {
        if (_externalState->shouldIgnoreAuthChecks())
            return true;

        for (size_t i = 0; i < privileges.size(); ++i) {
            if (!_isAuthorizedForPrivilege(privileges[i]))
                return false;
        }

        return true;
    }

    bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                ActionType action) {
        return isAuthorizedForPrivilege(Privilege(resource, action));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                const ActionSet& actions) {
        return isAuthorizedForPrivilege(Privilege(resource, actions));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 ActionType action) {
        return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), action));
    }

    bool AuthorizationSession::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                const ActionSet& actions) {
        return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), actions));
    }

    static const ResourcePattern anyUsersCollectionPattern = ResourcePattern::forCollectionName(
            "system.users");
    static const ResourcePattern anyProfileCollectionPattern = ResourcePattern::forCollectionName(
            "system.profile");
    static const ResourcePattern anyIndexesCollectionPattern = ResourcePattern::forCollectionName(
            "system.indexes");

    // Returns a new privilege that has replaced the actions needed to handle special casing
    // certain namespaces like system.users and system.profile.  Note that the special handling
    // of system.indexes takes place in checkAuthForInsert, not here.
    static Privilege _modifyPrivilegeForSpecialCases(Privilege privilege) {
        ActionSet newActions(privilege.getActions());
        const ResourcePattern& target(privilege.getResourcePattern());
        if (anyUsersCollectionPattern.matchesResourcePattern(target)) {
            if (newActions.contains(ActionType::insert) ||
                    newActions.contains(ActionType::update) ||
                    newActions.contains(ActionType::remove)) {
                // End users can't modify system.users directly, only the system can.
                newActions.addAction(ActionType::userAdminV1);
            } else {
                newActions.addAction(ActionType::userAdmin);
            }
            newActions.removeAction(ActionType::find);
            newActions.removeAction(ActionType::insert);
            newActions.removeAction(ActionType::update);
            newActions.removeAction(ActionType::remove);
        } else if (anyProfileCollectionPattern.matchesResourcePattern(target)) {
            newActions.removeAction(ActionType::find);
            newActions.addAction(ActionType::profileRead);
        } else if (anyIndexesCollectionPattern.matchesResourcePattern(target)
                   && newActions.contains(ActionType::find)) {
            newActions.removeAction(ActionType::find);
            newActions.addAction(ActionType::indexRead);
        }

        return Privilege(privilege.getResourcePattern(), newActions);
    }

    bool AuthorizationSession::_isAuthorizedForPrivilege(const Privilege& privilege) {
        AuthorizationManager& authMan = getAuthorizationManager();
        Privilege modifiedPrivilege = _modifyPrivilegeForSpecialCases(privilege);

        // Need to check not just the resource of the privilege, but also just the database
        // component and the "*" resource.
        ResourcePattern resourceSearchList[3];
        resourceSearchList[0] = ResourcePattern::forAnyResource();
        resourceSearchList[1] = modifiedPrivilege.getResourcePattern();
        if (modifiedPrivilege.getResourcePattern().isExactNamespacePattern()) {
            resourceSearchList[2] =
                ResourcePattern::forDatabaseName(modifiedPrivilege.getResourcePattern().ns().db);
        }


        ActionSet unmetRequirements = modifiedPrivilege.getActions();
        UserSet::iterator it = _authenticatedUsers.begin();
        while (it != _authenticatedUsers.end()) {
            User* user = *it;

            if (!user->isValid()) {
                // Make a good faith effort to acquire an up-to-date user object, since the one
                // we've cached is marked "out-of-date."
                UserName name = user->getName();
                User* updatedUser;

                Status status = authMan.acquireUser(name, &updatedUser);
                switch (status.code()) {
                case ErrorCodes::OK: {
                    // Success! Replace the old User object with the updated one.
                    fassert(17067, _authenticatedUsers.replaceAt(it, updatedUser) == user);
                    authMan.releaseUser(user);
                    user = updatedUser;
                    LOG(1) << "Updated session cache of user information for " << name;
                    break;
                }
                case ErrorCodes::UserNotFound: {
                    // User does not exist anymore; remove it from _authenticatedUsers.
                    fassert(17068, _authenticatedUsers.removeAt(it) == user);
                    authMan.releaseUser(user);
                    LOG(1) << "Removed deleted user " << name <<
                        " from session cache of user information.";
                    continue;  // No need to advance "it" in this case.
                }
                default:
                    // Unrecognized error; assume that it's transient, and continue working with the
                    // out-of-date privilege data.
                    warning() << "Could not fetch updated user privilege information for " <<
                        name << "; continuing to use old information.  Reason is " << status;
                    break;
                }
            }

            for (int i = 0; i < static_cast<int>(boost::size(resourceSearchList)); ++i) {
                ActionSet userActions = user->getActionsForResource(resourceSearchList[i]);
                unmetRequirements.removeAllActionsFromSet(userActions);

                if (unmetRequirements.empty())
                    return true;
            }
            ++it;
        }

        return false;
    }

} // namespace mongo
