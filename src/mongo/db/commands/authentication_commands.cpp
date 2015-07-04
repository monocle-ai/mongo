/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/db/commands/authentication_commands.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/mongo_authentication_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/util/md5.hpp"

namespace mongo {

    static bool _areNonceAuthenticateCommandsEnabled = true;
    static const char _nonceAuthenticateCommandsDisabledMessage[] =
        "Challenge-response authentication using getnonce and authenticate commands is disabled.";

    void CmdAuthenticate::disableCommand() { _areNonceAuthenticateCommandsEnabled = false; }

    /* authentication

       system.users contains
         { user : <username>, pwd : <pwd_digest>, ... }

       getnonce sends nonce to client

       client then sends { authenticate:1, nonce64:<nonce_str>, user:<username>, key:<key> }

       where <key> is md5(<nonce_str><username><pwd_digest_str>) as a string
    */

    class CmdGetNonce : public InformationCommand {
    public:
        CmdGetNonce() : InformationCommand("getnonce") {
            _random = SecureRandom::create();
        }

        virtual bool requiresAuth() { return false; }
        void help(stringstream& h) const { h << "internal"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        bool run(const string&, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            nonce64 n = _random->nextInt64();
            stringstream ss;
            ss << hex << n;
            result.append("nonce", ss.str() );
            ClientBasic::getCurrent()->resetAuthenticationSession(
                    new MongoAuthenticationSession(n));
            return true;
        }

        SecureRandom* _random;
    } cmdGetNonce;

    static void _auditFailedAuthentication(const string &dbname, const string &user) {
        audit::logAuthentication(ClientBasic::getCurrent(),
                                 dbname,
                                 "MONGODB-CR",
                                 user,
                                 ErrorCodes::AuthenticationFailed);
    }

    bool CmdAuthenticate::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

        log() << " authenticate db: " << dbname << " " << cmdObj << endl;

        string user = cmdObj.getStringField("user");

        if (!_areNonceAuthenticateCommandsEnabled) {
            // SERVER-8461, MONGODB-CR must be enabled for authenticating the internal user, so that
            // cluster members may communicate with each other.
            if (dbname != StringData("local", StringData::LiteralTag()) ||
                user != internalSecurity.user) {
                _auditFailedAuthentication(dbname, user);
                errmsg = _nonceAuthenticateCommandsDisabledMessage;
                result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
                return false;
            }
        }

        string key = cmdObj.getStringField("key");
        string received_nonce = cmdObj.getStringField("nonce");

        if( user.empty() || key.empty() || received_nonce.empty() ) {
            _auditFailedAuthentication(dbname, user);
            log() << "field missing/wrong type in received authenticate command "
                  << dbname
                  << endl;
            errmsg = "auth fails";
            sleepmillis(10);
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        stringstream digestBuilder;

        {
            bool reject = false;
            ClientBasic *client = ClientBasic::getCurrent();
            AuthenticationSession *session = client->getAuthenticationSession();
            if (!session || session->getType() != AuthenticationSession::SESSION_TYPE_MONGO) {
                reject = true;
                LOG(1) << "auth: No pending nonce" << endl;
            }
            else {
                nonce64 nonce = static_cast<MongoAuthenticationSession*>(session)->getNonce();
                digestBuilder << hex << nonce;
                reject = digestBuilder.str() != received_nonce;
                if ( reject ) {
                    LOG(1) << "auth: Authentication failed for " << dbname << '$' << user << endl;
                }
            }
            client->resetAuthenticationSession(NULL);

            if ( reject ) {
                _auditFailedAuthentication(dbname, user);
                log() << "auth: bad nonce received or getnonce not called. could be a driver bug or a security attack. db:" << dbname << endl;
                errmsg = "auth fails";
                sleepmillis(30);
                result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
                return false;
            }
        }

        BSONObj userObj;
        string pwd;
        Status status = ClientBasic::getCurrent()->getAuthorizationManager()->getPrivilegeDocument(
                dbname, PrincipalName(user, dbname), &userObj);

        // HACK: We added an external flag to the principal field for external authentication.  If
        // it is present here, then the user is trying to authenticate locally, and there is no
        // password stored locally for this user.  So we must exit.
        const bool isExternal = userObj.getBoolField("external");
        if (isExternal) {
            _auditFailedAuthentication(dbname, user);
            log() << "cannot locally authenticate an external user" << std::endl;
            errmsg = "auth fails";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }
                                 
        if (!status.isOK()) {
            _auditFailedAuthentication(dbname, user);
            log() << status.reason() << std::endl;
            errmsg = "auth fails";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }
        pwd = userObj["pwd"].String();

        md5digest d;
        {
            digestBuilder << user << pwd;
            string done = digestBuilder.str();

            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t *) done.c_str(), done.size());
            md5_finish(&st, d);
        }

        string computed = digestToString( d );

        const bool authenticated = key == computed;
        audit::logAuthentication(ClientBasic::getCurrent(),
                                 dbname,
                                 "MONGODB-CR",
                                 user,
                                 authenticated ? ErrorCodes::OK : ErrorCodes::AuthenticationFailed);
        if ( !authenticated ) {
            log() << "auth: key mismatch " << user << ", ns:" << dbname << endl;
            errmsg = "auth fails";
            result.append(saslCommandCodeFieldName, ErrorCodes::AuthenticationFailed);
            return false;
        }

        AuthorizationManager* authorizationManager =
            ClientBasic::getCurrent()->getAuthorizationManager();
        Principal* principal = new Principal(PrincipalName(user, dbname));
        principal->setImplicitPrivilegeAcquisition(true);
        authorizationManager->addAuthorizedPrincipal(principal);

        result.append( "dbname" , dbname );
        result.append( "user" , user );
        return true;
    }
    CmdAuthenticate cmdAuthenticate;

    class CmdLogout : public InformationCommand {
    public:
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        void help(stringstream& h) const { h << "de-authenticate"; }
        CmdLogout() : InformationCommand("logout") {}
        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
            AuthorizationManager* authManager = ClientBasic::getCurrent()->getAuthorizationManager();
            authManager->logoutDatabase(dbname);
            return true;
        }
    } cmdLogout;
}
