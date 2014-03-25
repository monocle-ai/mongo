/**
*    Copyright (C) 2013 Tokutek Inc.
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

namespace mongo {

    namespace storage {

        class ASSERT_IDS {
        public:
            static const int AmbiguousFieldNames = 15855;
            static const int CannotHashArrays = 16897;
            static const int ParallelArrays = 10888;
            static const int LockDeadlock = 16760;
            static const int CapPartitionFailed = 17248;
            static const int TxnNotFoundOnCommit = 16788; // uassert(16788, "no transaction exists to be committed", cc().hasTxn());
        };

    } // namespace storage

} // namespace mongo
