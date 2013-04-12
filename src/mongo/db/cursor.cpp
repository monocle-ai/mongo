/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/curop.h"
#include "mongo/db/cursor.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    Cursor *BasicCursor::make( NamespaceDetails *d, int direction ) {
        if ( d != NULL ) {
            return new BasicCursor(d, direction);
        } else {
            return new DummyCursor(direction);
        }
    }

    BasicCursor::BasicCursor( NamespaceDetails *d, int direction ) :
        _c( d, d->getPKIndex(),
            direction > 0 ? minKey : maxKey, // start at the beginning for forward cursor
            direction > 0 ? maxKey : minKey, // finish at the end for forward cursor
            true, // end key is inclusive, because we want to scan everything.
            direction ),
        _direction(direction) {
    }

} // namespace mongo
