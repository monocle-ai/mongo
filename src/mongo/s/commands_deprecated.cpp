// dbcommands_deprecated.cpp

/**
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

#include "mongo/pch.h"

#include "mongo/db/commands.h"

namespace mongo {

    class ConvertToCappedCmd : public DeprecatedCommand  {
      public:
        ConvertToCappedCmd() : DeprecatedCommand("convertToCapped") {}
    } convertToCappedCmd;


    class RepairDatabaseCmd : public DeprecatedCommand {
      public:
        RepairDatabaseCmd() :  DeprecatedCommand("repairDatabase") {}
    } repairDatabaseCmd;

    class CollectionModCmd : public DeprecatedCommand {
      public:
        CollectionModCmd() :  DeprecatedCommand("collMod") {}
    } collectionModCmd;


    class ApplyOpsCmd : public DeprecatedCommand {
      public:
        ApplyOpsCmd() : DeprecatedCommand("applyOps") {}
    } applyOpsCmd;

    class CompactCmd : public DeprecatedCommand {
      public:
        CompactCmd() : DeprecatedCommand("compact") {}
    } compactCmd;

} // namespace mongo
