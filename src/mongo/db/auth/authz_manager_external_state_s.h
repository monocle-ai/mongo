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

#pragma once

#include <boost/function.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/client/distlock.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

    /**
     * The implementation of AuthzManagerExternalState functionality for mongos.
     */
    class AuthzManagerExternalStateMongos : public AuthzManagerExternalState{
        MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMongos);

    public:
        AuthzManagerExternalStateMongos();
        virtual ~AuthzManagerExternalStateMongos();

        virtual Status insertPrivilegeDocument(const std::string& dbname,
                                               const BSONObj& userObj,
                                               const BSONObj& writeConcern);

        virtual Status updatePrivilegeDocument(const UserName& user,
                                               const BSONObj& updateObj,
                                               const BSONObj& writeConcern);

        virtual Status removePrivilegeDocuments(const BSONObj& query,
                                                const BSONObj& writeConcern,
                                                int* numRemoved);

        virtual Status getAllDatabaseNames(std::vector<std::string>* dbnames);

        virtual Status getAllV1PrivilegeDocsForDB(const std::string& dbname,
                                                  std::vector<BSONObj>* privDocs);

        virtual Status findOne(const NamespaceString& collectionName,
                               const BSONObj& query,
                               BSONObj* result);
        virtual Status query(const NamespaceString& collectionName,
                             const BSONObj& query,
                             const boost::function<void(const BSONObj&)>& resultProcessor);
        virtual Status insert(const NamespaceString& collectionName,
                              const BSONObj& document,
                              const BSONObj& writeConcern);
        virtual Status updateOne(const NamespaceString& collectionName,
                                 const BSONObj& query,
                                 const BSONObj& updatePattern,
                                 bool upsert,
                                 const BSONObj& writeConcern);
        virtual Status remove(const NamespaceString& collectionName,
                              const BSONObj& query,
                              const BSONObj& writeConcern);
        virtual Status createIndex(const NamespaceString& collectionName,
                                   const BSONObj& pattern,
                                   bool unique,
                                   const BSONObj& writeConcern);
        virtual Status dropCollection(const NamespaceString& collectionName,
                                      const BSONObj& writeConcern);
        virtual Status renameCollection(const NamespaceString& oldName,
                                        const NamespaceString& newName,
                                        const BSONObj& writeConcern);
        virtual Status copyCollection(const NamespaceString& fromName,
                                      const NamespaceString& toName,
                                      const BSONObj& writeConcern);
        virtual bool tryAcquireAuthzUpdateLock(const StringData& why);
        virtual void releaseAuthzUpdateLock();

    protected:
        virtual Status _findUser(const string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result);

    private:
        boost::mutex _distLockGuard; // Guards access to _authzDataUpdateLock
        scoped_ptr<ScopedDistributedLock> _authzDataUpdateLock;
    };

} // namespace mongo
