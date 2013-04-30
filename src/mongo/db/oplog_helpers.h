/**
*    Copyright (C) 2012 Tokutek Inc.
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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class TxnContext;

// helpers for opLog stuff
namespace OpLogHelpers{

    void logComment(BSONObj comment, TxnContext* txn);
    void logInsert(const char* ns, BSONObj row, TxnContext* txn);
    void logUpdate(const char* ns, const BSONObj& pk, const BSONObj& oldRow, const BSONObj& newRow, bool fromMigrate, TxnContext* txn);
    void logDelete(const char* ns, BSONObj row, bool fromMigrate, TxnContext* txn);
    void logCommand(const char* ns, BSONObj row, TxnContext* txn);
    void applyOperationFromOplog(const BSONObj& op);
}


} // namespace mongo

