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

#include "mongo/db/auth/auth_external_state.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"

namespace mongo {

    static const char USER_FIELD[] = "user";
    static const char USER_SOURCE_FIELD[] = "userSource";
    static const char PASSWORD_FIELD[] = "pwd";

    AuthExternalState::AuthExternalState() {}
    AuthExternalState::~AuthExternalState() {}

    Status AuthExternalState::getPrivilegeDocumentOverConnection(DBClientBase* conn,
                                                                 const std::string& dbname,
                                                                 const PrincipalName& principalName,
                                                                 BSONObj* result) {
        if (principalName.getUser() == internalSecurity.user) {
            if (internalSecurity.pwd.empty()) {
                return Status(ErrorCodes::UserNotFound,
                              "key file must be used to log in with internal user",
                              15889);
            }
            *result = BSON(USER_FIELD << internalSecurity.user <<
                           PASSWORD_FIELD << internalSecurity.pwd).getOwned();
            return Status::OK();
        }

        std::string usersNamespace = dbname + ".system.users";

        Client::ReadContext tc(usersNamespace, dbpath, false);
        // we want all authentication stuff to happen on an alternate stack
        Client::AlternateTransactionStack altStack;
        Client::Transaction txn(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);

        BSONObj userBSONObj;
        BSONObjBuilder queryBuilder;
        queryBuilder.append(USER_FIELD, principalName.getUser());
        if (principalName.getDB() == dbname) {
            queryBuilder.appendNull(USER_SOURCE_FIELD);
        }
        else {
            queryBuilder.append(USER_SOURCE_FIELD, principalName.getDB());
        }
        userBSONObj = conn->findOne(usersNamespace, queryBuilder.obj(), 0, QueryOption_SlaveOk);
        if (userBSONObj.isEmpty()) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "auth: couldn't find user " <<
                          principalName.toString() << ", " << usersNamespace,
                          0);
        }

        txn.commit();

        *result = userBSONObj.getOwned();
        return Status::OK();
    }

}  // namespace mongo
